// SPDX-License-Identifier: MPL-2.0
package mapper.texturedump;

import cpw.mods.fml.common.FMLCommonHandler;
import cpw.mods.fml.common.Mod;
import cpw.mods.fml.common.event.FMLInitializationEvent;
import cpw.mods.fml.common.eventhandler.SubscribeEvent;
import cpw.mods.fml.common.gameevent.TickEvent;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import javax.imageio.ImageIO;
import org.lwjgl.BufferUtils;
import org.lwjgl.opengl.GL11;

// dumps the top face texture of every block and meta, for minecraft-mapper's textured view
//
// 1.7.10 has no block models on disk, textures are picked in code by Block.getIcon(side, meta),
// so the only reliable source is the running game. once the main menu is up this writes
// texture-dump/atlas.png (every distinct top face, 16x16 each) and texture-dump/blocks.json
@Mod(modid = "mapper-texturedump", name = "Mapper Texture Dump", version = "1.0", acceptableRemoteVersions = "*")
public class TextureDump {
  static final int ICON = 16, COLUMNS = 64, TOP = 1;
  boolean done = false;
  // every named item stack dumpItems saw, foods are picked from these
  final List<Object> allStacks = new ArrayList<>();
  int ticks = 0;

  @Mod.EventHandler
  public void init(FMLInitializationEvent e) {
    if (FMLCommonHandler.instance().getSide().isClient()) FMLCommonHandler.instance().bus().register(this);
  }

  @SubscribeEvent
  public void tick(TickEvent.ClientTickEvent e) {
    if (done || e.phase != TickEvent.Phase.END) return;
    try {
      Object mc = callStatic(Class.forName("net.minecraft.client.Minecraft"), "func_71410_x", "getMinecraft");
      // wait for the main menu, the block atlas is stitched by then
      if (get(mc, "field_71462_r", "currentScreen") == null || ++ticks < 40) return;
      done = true;
      dump(mc);
      // set by tools/dump-textures.sh, which starts the client only for this
      if (Boolean.getBoolean("texturedump.exit")) FMLCommonHandler.instance().exitJava(0, false);
    } catch (Throwable t) {
      done = true;
      System.err.println("[texturedump] failed");
      t.printStackTrace();
    }
  }

  void dump(Object mc) throws Exception {
    long start = System.currentTimeMillis();
    Class<?> textureMap = Class.forName("net.minecraft.client.renderer.texture.TextureMap");
    Object location = getStatic(textureMap, "field_110575_b", "locationBlocksTexture");
    Object textures = call(mc, "func_110434_K", "getTextureManager");
    Object atlasTex = call(textures, "func_110581_b", "getTexture", location);
    int glId = (Integer) call(atlasTex, "func_110552_b", "getGlTextureId");

    GL11.glBindTexture(GL11.GL_TEXTURE_2D, glId);
    int w = GL11.glGetTexLevelParameteri(GL11.GL_TEXTURE_2D, 0, GL11.GL_TEXTURE_WIDTH);
    int h = GL11.glGetTexLevelParameteri(GL11.GL_TEXTURE_2D, 0, GL11.GL_TEXTURE_HEIGHT);
    System.out.println("[texturedump] block atlas is " + w + "x" + h);
    ByteBuffer atlas = BufferUtils.createByteBuffer(w * h * 4);
    GL11.glGetTexImage(GL11.GL_TEXTURE_2D, 0, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, atlas);

    Object registry = callStatic(Class.forName("cpw.mods.fml.common.registry.GameData"), "getBlockRegistry", "getBlockRegistry");
    Method nameOf = method(registry.getClass(), "func_148750_c", "getNameForObject", Object.class);

    Map<String, Integer> iconIndex = new HashMap<>();
    List<int[]> icons = new ArrayList<>();  // x0, y0, x1, y1 in atlas pixels
    StringBuilder json = new StringBuilder("{\n");
    int blocks = 0;
    for (Object block : (Iterable<?>) call(registry, "typeSafeIterable", "typeSafeIterable")) {
      String name = String.valueOf(nameOf.invoke(registry, block));
      int render = -1;
      try { render = (Integer) call(block, "func_149645_b", "getRenderType"); } catch (Throwable t) {}

      StringBuilder metas = new StringBuilder();
      for (int meta = 0; meta < 16; meta++) {
        Object icon;
        int tint = 0xffffff;
        try {
          icon = call(block, "func_149691_a", "getIcon", TOP, meta);
          tint = (Integer) call(block, "func_149741_i", "getRenderColor", meta);
        } catch (Throwable t) {
          continue;  // plenty of mods throw for metas they don't use
        }
        if (icon == null) continue;
        float u0 = (Float) call(icon, "func_94209_e", "getMinU"), u1 = (Float) call(icon, "func_94212_f", "getMaxU");
        float v0 = (Float) call(icon, "func_94206_g", "getMinV"), v1 = (Float) call(icon, "func_94210_h", "getMaxV");
        int[] rect = { Math.round(u0 * w), Math.round(v0 * h), Math.round(u1 * w), Math.round(v1 * h) };
        if (rect[2] <= rect[0] || rect[3] <= rect[1]) continue;
        String key = rect[0] + "," + rect[1] + "," + rect[2] + "," + rect[3];
        Integer index = iconIndex.get(key);
        if (index == null) {
          index = icons.size();
          iconIndex.put(key, index);
          icons.add(rect);
        }
        if (metas.length() > 0) metas.append(",");
        metas.append("\"").append(meta).append("\":{\"icon\":").append(index).append(",\"tint\":").append(tint & 0xffffff).append("}");
      }
      if (metas.length() == 0) continue;
      if (blocks++ > 0) json.append(",\n");
      json.append("  \"").append(escape(name)).append("\":{\"render\":").append(render).append(",\"metas\":{").append(metas).append("}}");
    }
    json.append("\n}\n");

    // every distinct top face, scaled to 16x16 with nearest sampling (some packs use 32x or more)
    int rows = (icons.size() + COLUMNS - 1) / COLUMNS;
    BufferedImage out = new BufferedImage(COLUMNS * ICON, Math.max(1, rows) * ICON, BufferedImage.TYPE_INT_ARGB);
    for (int i = 0; i < icons.size(); i++) {
      int[] r = icons.get(i);
      int ox = (i % COLUMNS) * ICON, oy = (i / COLUMNS) * ICON;
      for (int y = 0; y < ICON; y++) {
        for (int x = 0; x < ICON; x++) {
          int sx = r[0] + x * (r[2] - r[0]) / ICON, sy = r[1] + y * (r[3] - r[1]) / ICON;
          int p = (sy * w + sx) * 4;
          int rgba = ((atlas.get(p + 3) & 0xff) << 24) | ((atlas.get(p) & 0xff) << 16) | ((atlas.get(p + 1) & 0xff) << 8) | (atlas.get(p + 2) & 0xff);
          out.setRGB(ox + x, oy + y, rgba);
        }
      }
    }

    File dir = new File("texture-dump");
    dir.mkdirs();
    ImageIO.write(out, "png", new File(dir, "atlas.png"));
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "blocks.json")), StandardCharsets.UTF_8)) {
      wr.write("{\"icon_size\":" + ICON + ",\"columns\":" + COLUMNS + ",\"blocks\":" + json + "}\n");
    }
    System.out.println("[texturedump] wrote " + blocks + " blocks, " + icons.size() + " icons to " + dir.getAbsolutePath() +
                       " in " + (System.currentTimeMillis() - start) + "ms");
    dumpItems(dir);
  }

  // texture-dump/items.json: registry name -> damage -> display name, for every variant the creative menu
  // lists. GregTech keeps whole material sets in one item, the name only exists in game ("Iron Ingot")
  void dumpItems(File dir) throws Exception {
    Object registry = callStatic(Class.forName("cpw.mods.fml.common.registry.GameData"), "getItemRegistry", "getItemRegistry");
    Method nameOf = method(registry.getClass(), "func_148750_c", "getNameForObject", Object.class);
    Class<?> stackClass = Class.forName("net.minecraft.item.ItemStack");
    StringBuilder json = new StringBuilder("{\n");
    int items = 0, names = 0;
    for (Object item : (Iterable<?>) call(registry, "typeSafeIterable", "typeSafeIterable")) {
      String name = String.valueOf(nameOf.invoke(registry, item));
      List<Object> stacks = new ArrayList<>();
      try {
        Object tab = call(item, "func_77640_w", "getCreativeTab");
        if (tab != null) {
          Method sub = null;
          for (Class<?> k = item.getClass(); k != null && sub == null; k = k.getSuperclass())
            for (Method m : k.getDeclaredMethods())
              if ((m.getName().equals("func_150895_a") || m.getName().equals("getSubItems")) && m.getParameterTypes().length == 3) sub = m;
          if (sub != null) {
            sub.setAccessible(true);
            sub.invoke(item, item, tab, stacks);
          }
        }
      } catch (Throwable t) {}
      // items outside the creative menu still get their plain damage 0 name
      if (stacks.isEmpty()) {
        try { stacks.add(stackClass.getConstructor(Class.forName("net.minecraft.item.Item"), int.class, int.class).newInstance(item, 1, 0)); } catch (Throwable t) {}
      }
      // GregTech and Bartworks meta items name every material x prefix, but the creative menu only lists some
      // (crushed ore of a material GT hides there, for one). ask for every damage value, a name comes from the
      // material and prefix, an invalid one is empty, an untranslated key, or for Bartworks a "_NULL" material
      // a programmed circuit's configurations each have their own picture, the creative menu only lists the first
      if (name.equals("gregtech:gt.integrated_circuit")) {
        try {
          java.lang.reflect.Constructor<?> make = stackClass.getConstructor(Class.forName("net.minecraft.item.Item"), int.class, int.class);
          for (int dmg = 1; dmg <= 24; dmg++) stacks.add(make.newInstance(item, 1, dmg));
        } catch (Throwable t) {}
      }
      if (name.startsWith("gregtech:gt.metaitem.") || name.startsWith("bartworks:gt.bwMetaGenerated")) {
        try {
          java.lang.reflect.Constructor<?> make = stackClass.getConstructor(Class.forName("net.minecraft.item.Item"), int.class, int.class);
          for (int dmg = 0; dmg < 32767; dmg++) stacks.add(make.newInstance(item, 1, dmg));
        } catch (Throwable t) {}
      }
      Map<Integer, String> byDamage = new java.util.TreeMap<>();
      for (Object stack : stacks) {
        try {
          int damage = (Integer) call(stack, "func_77960_j", "getItemDamage");
          String display = (String) call(stack, "func_82833_r", "getDisplayName");
          // Bartworks names materials it doesn't have "_NULL"
          if (display != null && !display.trim().isEmpty() && !display.endsWith(".name") && !display.contains("_NULL") && !byDamage.containsKey(damage)) {
            byDamage.put(damage, display);
            allStacks.add(stack);
          }
        } catch (Throwable t) {}
      }
      if (byDamage.isEmpty()) continue;
      if (items++ > 0) json.append(",\n");
      json.append("  \"").append(escape(name)).append("\":{");
      boolean first = true;
      for (Map.Entry<Integer, String> e : byDamage.entrySet()) {
        if (!first) json.append(",");
        first = false;
        json.append("\"").append(e.getKey()).append("\":\"").append(escape(e.getValue())).append("\"");
        names++;
      }
      json.append("}");
    }
    json.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "items.json")), StandardCharsets.UTF_8)) {
      wr.write(json.toString());
    }
    System.out.println("[texturedump] wrote " + names + " names of " + items + " items");
    dumpGtMachines(dir, nameOf, registry);
    dumpCokeOven(dir, nameOf, registry);
    dumpFoods(dir, nameOf, registry);
    dumpFluids(dir);
    try {
      dumpIcons(dir, nameOf, registry);
    } catch (Throwable t) {
      System.err.println("[texturedump] item icons failed");
      t.printStackTrace();
    }
  }

  // texture-dump/item_icons.png and item_icons.json: every named item (and every fluid) drawn by the game itself
  // at 16x16, the way an inventory shows it: 3D blocks, GregTech's tinted layers, custom renderers. identical
  // pictures share one cell. json: {"size":16,"columns":n,"icons":{"mod:name:damage":cell,"fluid:name":cell}}
  void dumpIcons(File dir, Method nameOf, Object itemRegistry) throws Exception {
    final int S = 1024, CELL = 16, PER_ROW = S / CELL, BATCH = PER_ROW * PER_ROW, COLS = 128;
    Object mc = callStatic(Class.forName("net.minecraft.client.Minecraft"), "func_71410_x", "getMinecraft");
    Object font = get(mc, "field_71466_p", "fontRenderer");
    Object textures = call(mc, "func_110434_K", "getTextureManager");
    Class<?> fbClass = Class.forName("net.minecraft.client.shader.Framebuffer");
    Object fb = fbClass.getConstructor(int.class, int.class, boolean.class).newInstance(S, S, true);
    Object renderItem = Class.forName("net.minecraft.client.renderer.entity.RenderItem").getConstructor().newInstance();
    Method draw = null;
    for (Method m : renderItem.getClass().getMethods())
      if ((m.getName().equals("func_82406_b") || m.getName().equals("renderItemAndEffectIntoGUI")) && m.getParameterTypes().length == 5) draw = m;
    Class<?> lighting = Class.forName("net.minecraft.client.renderer.RenderHelper");
    Object blockAtlas = getStatic(Class.forName("net.minecraft.client.renderer.texture.TextureMap"), "field_110575_b", "locationBlocksTexture");
    Object tess = getStatic(Class.forName("net.minecraft.client.renderer.Tessellator"), "field_78398_a", "instance");
    // chests and other blocks drawn by their tile entity renderer: its dispatcher only gets the texture manager and
    // font when a world loads, at the main menu it has none and draws with whatever texture is bound (the block
    // atlas, every chest came out as noise)
    try {
      Object dispatcher = getStatic(Class.forName("net.minecraft.client.renderer.tileentity.TileEntityRendererDispatcher"), "field_147556_a", "instance");
      field(dispatcher.getClass(), "field_147553_e", "field_147553_e").set(dispatcher, textures);
      field(dispatcher.getClass(), "field_147557_n", "field_147557_n").set(dispatcher, font);
    } catch (Throwable t) {
      System.err.println("[texturedump] couldn't give the tile entity renderers a texture manager: " + t);
    }

    // what to draw: item stacks, and fluids as their still texture tinted with their color
    List<String> keys = new ArrayList<>();
    List<Object> what = new ArrayList<>();
    java.util.Set<String> seenKeys = new java.util.HashSet<>();
    for (Object st : allStacks) {
      String k = key(st, nameOf, itemRegistry);
      if (k != null && seenKeys.add(k)) { keys.add(k); what.add(st); }
    }
    Map<?, ?> fluids = (Map<?, ?>) Class.forName("net.minecraftforge.fluids.FluidRegistry").getMethod("getRegisteredFluids").invoke(null);
    for (Map.Entry<?, ?> e : fluids.entrySet()) { keys.add("fluid:" + e.getKey()); what.add(e.getValue()); }

    Map<java.nio.IntBuffer, Integer> cellOf = new HashMap<>();
    List<int[]> cells = new ArrayList<>();
    StringBuilder json = new StringBuilder();
    ByteBuffer px = BufferUtils.createByteBuffer(S * S * 4);
    int drawn = 0, failed = 0;

    GL11.glMatrixMode(GL11.GL_PROJECTION);
    GL11.glPushMatrix();
    GL11.glMatrixMode(GL11.GL_MODELVIEW);
    GL11.glPushMatrix();
    // the GL stacks as they should be between items, a renderer that throws halfway leaves its pushes behind
    final int modelDepth = GL11.glGetInteger(GL11.GL_MODELVIEW_STACK_DEPTH), projDepth = GL11.glGetInteger(GL11.GL_PROJECTION_STACK_DEPTH);
    final int texDepth = GL11.glGetInteger(GL11.GL_TEXTURE_STACK_DEPTH), attribDepth = GL11.glGetInteger(GL11.GL_ATTRIB_STACK_DEPTH);
    final Object fbf = fb, tessf = tess, renderItemf = renderItem;
    Runnable setup = () -> {
      // unwind whatever a failed renderer pushed, back to texture unit 0 (tile entity renderers switch to the lightmap's)
      while (GL11.glGetInteger(GL11.GL_ATTRIB_STACK_DEPTH) > attribDepth) GL11.glPopAttrib();
      org.lwjgl.opengl.GL13.glActiveTexture(org.lwjgl.opengl.GL13.GL_TEXTURE0);
      org.lwjgl.opengl.GL13.glClientActiveTexture(org.lwjgl.opengl.GL13.GL_TEXTURE0);
      GL11.glMatrixMode(GL11.GL_TEXTURE);
      while (GL11.glGetInteger(GL11.GL_TEXTURE_STACK_DEPTH) > texDepth) GL11.glPopMatrix();
      GL11.glLoadIdentity();
      GL11.glMatrixMode(GL11.GL_PROJECTION);
      while (GL11.glGetInteger(GL11.GL_PROJECTION_STACK_DEPTH) > projDepth) GL11.glPopMatrix();
      GL11.glLoadIdentity();
      GL11.glOrtho(0, S, S, 0, 1000, 3000);
      GL11.glMatrixMode(GL11.GL_MODELVIEW);
      while (GL11.glGetInteger(GL11.GL_MODELVIEW_STACK_DEPTH) > modelDepth) GL11.glPopMatrix();
      GL11.glLoadIdentity();
      GL11.glTranslatef(0, 0, -2000);
      try { call(fbf, "func_147610_a", "bindFramebuffer", true); } catch (Exception e) {}
      GL11.glViewport(0, 0, S, S);
      // a shader program or buffer some renderer left bound turns every fixed function draw after it blank
      org.lwjgl.opengl.GL20.glUseProgram(0);
      org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL15.GL_ARRAY_BUFFER, 0);
      org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL15.GL_ELEMENT_ARRAY_BUFFER, 0);
      try { org.lwjgl.opengl.GL30.glBindVertexArray(0); } catch (Throwable t) {}
      for (int arr : new int[] { GL11.GL_VERTEX_ARRAY, GL11.GL_COLOR_ARRAY, GL11.GL_NORMAL_ARRAY, GL11.GL_TEXTURE_COORD_ARRAY })
        GL11.glDisableClientState(arr);
      GL11.glColorMask(true, true, true, true);
      GL11.glDepthMask(true);
      GL11.glDisable(GL11.GL_SCISSOR_TEST);
      GL11.glEnable(GL11.GL_TEXTURE_2D);
      GL11.glEnable(GL11.GL_DEPTH_TEST);
      GL11.glDepthFunc(GL11.GL_LEQUAL);
      GL11.glEnable(GL11.GL_ALPHA_TEST);
      GL11.glAlphaFunc(GL11.GL_GREATER, 0.1f);
      GL11.glEnable(org.lwjgl.opengl.GL12.GL_RESCALE_NORMAL);
      GL11.glColor4f(1, 1, 1, 1);
      // java side state renderers leave behind: the tessellator's offset (a block renderer that set it and
      // never cleared it moves everything after it off the picture), the item renderer's depth
      try {
        Field drawing = field(tessf.getClass(), "field_78415_z", "isDrawing");
        if (drawing.getBoolean(tessf)) {
          drawing.setBoolean(tessf, false);
          method(tessf.getClass(), "func_78379_d", "reset").invoke(tessf);
        }
        method(tessf.getClass(), "func_78373_b", "setTranslation", double.class, double.class, double.class).invoke(tessf, 0.0, 0.0, 0.0);
      } catch (Throwable t) {}
      try { field(renderItemf.getClass(), "field_77023_b", "zLevel").setFloat(renderItemf, 0f); } catch (Throwable t) {}
    };
    for (int start = 0; start < keys.size(); start += BATCH) {
      setup.run();
      GL11.glClearColor(0, 0, 0, 0);
      GL11.glClear(GL11.GL_COLOR_BUFFER_BIT | GL11.GL_DEPTH_BUFFER_BIT);
      int end = Math.min(keys.size(), start + BATCH);
      for (int i = start; i < end; i++) {
        int x = ((i - start) % PER_ROW) * CELL, y = ((i - start) / PER_ROW) * CELL;
        Object w = what.get(i);
        // every item starts from the same state: some renderers change state and don't put it back
        // (disable textures, a blend mode...), every item after would draw blank. setup unwinds this push too
        setup.run();
        GL11.glPushAttrib(GL11.GL_ALL_ATTRIB_BITS);
        try {
          if (keys.get(i).startsWith("fluid:")) {
            Object icon = w.getClass().getMethod("getIcon").invoke(w);
            if (icon == null) continue;
            int color = (Integer) w.getClass().getMethod("getColor").invoke(w);
            callStatic(lighting, "func_74518_a", "disableStandardItemLighting");
            GL11.glDisable(GL11.GL_LIGHTING);
            call(textures, "func_110577_a", "bindTexture", blockAtlas);
            GL11.glColor4f(((color >> 16) & 255) / 255f, ((color >> 8) & 255) / 255f, (color & 255) / 255f, 1f);
            float u0 = (Float) call(icon, "func_94209_e", "getMinU"), u1 = (Float) call(icon, "func_94212_f", "getMaxU");
            float v0 = (Float) call(icon, "func_94206_g", "getMinV"), v1 = (Float) call(icon, "func_94210_h", "getMaxV");
            call(tess, "func_78382_b", "startDrawingQuads");
            call(tess, "func_78374_a", "addVertexWithUV", (double) x, (double) (y + CELL), 0.0, (double) u0, (double) v1);
            call(tess, "func_78374_a", "addVertexWithUV", (double) (x + CELL), (double) (y + CELL), 0.0, (double) u1, (double) v1);
            call(tess, "func_78374_a", "addVertexWithUV", (double) (x + CELL), (double) y, 0.0, (double) u1, (double) v0);
            call(tess, "func_78374_a", "addVertexWithUV", (double) x, (double) y, 0.0, (double) u0, (double) v0);
            call(tess, "func_78381_a", "draw");
            GL11.glColor4f(1, 1, 1, 1);
          } else {
            callStatic(lighting, "func_74520_c", "enableGUIStandardItemLighting");
            draw.invoke(renderItem, font, textures, w, x, y);
          }
        } catch (Throwable t) {
          if (failed++ < 3) {
            System.err.println("[texturedump] drawing " + keys.get(i) + " failed:");
            Throwable root = t;
            while (root.getCause() != null) root = root.getCause();
            System.err.println("[texturedump]   " + root);
            StackTraceElement[] st = root.getStackTrace();
            for (int j = 0; j < Math.min(4, st.length); j++) System.err.println("[texturedump]     at " + st[j]);
          }
          // one that threw mid draw leaves the shared tessellator drawing, every draw after would throw
          try {
            Field drawing = field(tess.getClass(), "field_78415_z", "isDrawing");
            if (drawing.getBoolean(tess)) {
              drawing.setBoolean(tess, false);
              method(tess.getClass(), "func_78379_d", "reset").invoke(tess);
            }
          } catch (Throwable t2) {}
        }
      }
      setup.run();
      GL11.glReadPixels(0, 0, S, S, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, px);
      for (int i = start; i < end; i++) {
        int x0 = ((i - start) % PER_ROW) * CELL, y0 = ((i - start) / PER_ROW) * CELL;
        int[] cell = new int[CELL * CELL];
        boolean any = false;
        for (int y = 0; y < CELL; y++)
          for (int x = 0; x < CELL; x++) {
            // rows come bottom up
            int p = ((S - 1 - (y0 + y)) * S + x0 + x) * 4;
            int a = px.get(p + 3) & 0xff;
            cell[y * CELL + x] = (a << 24) | ((px.get(p) & 0xff) << 16) | ((px.get(p + 1) & 0xff) << 8) | (px.get(p + 2) & 0xff);
            any |= a != 0;
          }
        if (!any) continue;
        java.nio.IntBuffer kb = java.nio.IntBuffer.wrap(cell);
        Integer c = cellOf.get(kb);
        if (c == null) {
          c = cells.size();
          cellOf.put(kb, c);
          cells.add(cell);
        }
        if (drawn++ > 0) json.append(",\n");
        json.append("  \"").append(escape(keys.get(i))).append("\":").append(c);
      }
    }
    callStatic(lighting, "func_74518_a", "disableStandardItemLighting");
    GL11.glMatrixMode(GL11.GL_PROJECTION);
    GL11.glPopMatrix();
    GL11.glMatrixMode(GL11.GL_MODELVIEW);
    GL11.glPopMatrix();
    call(fb, "func_147608_a", "deleteFramebuffer");
    Object main = call(mc, "func_147110_a", "getFramebuffer");
    call(main, "func_147610_a", "bindFramebuffer", true);

    int rows = (cells.size() + COLS - 1) / COLS;
    BufferedImage out = new BufferedImage(COLS * CELL, Math.max(1, rows) * CELL, BufferedImage.TYPE_INT_ARGB);
    for (int i = 0; i < cells.size(); i++)
      out.setRGB((i % COLS) * CELL, (i / COLS) * CELL, CELL, CELL, cells.get(i), 0, CELL);
    ImageIO.write(out, "png", new File(dir, "item_icons.png"));
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "item_icons.json")), StandardCharsets.UTF_8)) {
      wr.write("{\"size\":" + CELL + ",\"columns\":" + COLS + ",\"icons\":{\n" + json + "\n}}\n");
    }
    System.out.println("[texturedump] drew " + drawn + " of " + keys.size() + " item and fluid icons, " + cells.size() + " distinct, " + failed + " failed");
  }

  // texture-dump/fluids.json: fluid registry name -> its name in game ("bioethanol" is "Ethanol")
  void dumpFluids(File dir) throws Exception {
    Class<?> fr = Class.forName("net.minecraftforge.fluids.FluidRegistry");
    Map<?, ?> fluids = (Map<?, ?>) fr.getMethod("getRegisteredFluids").invoke(null);
    Class<?> fsc = Class.forName("net.minecraftforge.fluids.FluidStack"), fc = Class.forName("net.minecraftforge.fluids.Fluid");
    StringBuilder json = new StringBuilder("{\n");
    int n = 0;
    for (Map.Entry<?, ?> e : fluids.entrySet()) {
      String name = null;
      try {
        Object stack = fsc.getConstructor(fc, int.class).newInstance(e.getValue(), 1000);
        name = (String) fc.getMethod("getLocalizedName", fsc).invoke(e.getValue(), stack);
      } catch (Throwable t) {}
      if (name == null) try { name = (String) fc.getMethod("getLocalizedName").invoke(e.getValue()); } catch (Throwable t) {}
      if (name == null || name.isEmpty()) continue;
      if (n++ > 0) json.append(",\n");
      json.append("  \"").append(escape(String.valueOf(e.getKey()))).append("\":\"").append(escape(name)).append("\"");
    }
    json.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "fluids.json")), StandardCharsets.UTF_8)) {
      wr.write(json.toString());
    }
    System.out.println("[texturedump] wrote " + n + " fluid names");
  }

  // texture-dump/gt_machines.json: GregTech meta tile entity id (a machine's "mID") -> display name
  void dumpGtMachines(File dir, Method nameOf, Object itemRegistry) throws Exception {
    Object[] mtes;
    try {
      mtes = (Object[]) Class.forName("gregtech.api.GregTechAPI").getField("METATILEENTITIES").get(null);
    } catch (Throwable t) {
      System.out.println("[texturedump] no GregTech, skipping gt_machines.json");
      return;
    }
    StringBuilder json = new StringBuilder("{\n");
    int n = 0;
    for (int id = 0; id < mtes.length; id++) {
      if (mtes[id] == null) continue;
      String name = null;
      try { name = (String) mtes[id].getClass().getMethod("getLocalName").invoke(mtes[id]); } catch (Throwable t) {}
      if (name == null) try { name = (String) mtes[id].getClass().getMethod("getMetaName").invoke(mtes[id]); } catch (Throwable t) {}
      if (name == null) continue;
      if (n++ > 0) json.append(",\n");
      json.append("  \"").append(id).append("\":\"").append(escape(name)).append("\"");
    }
    json.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "gt_machines.json")), StandardCharsets.UTF_8)) {
      wr.write(json.toString());
    }
    System.out.println("[texturedump] wrote " + n + " GregTech machine names");

    // texture-dump/gt_machine_maps.json: mID -> the recipe maps it runs and its voltage tier (-1 for multiblocks,
    // whose voltage depends on their hatches), so a team's machines say which GT recipes it can run
    StringBuilder maps = new StringBuilder("{\n");
    int m = 0;
    Class<?> workable = null;
    try { workable = Class.forName("gregtech.api.interfaces.tileentity.RecipeMapWorkable"); } catch (Throwable t) {}
    for (int id = 0; workable != null && id < mtes.length; id++) {
      Object mte = mtes[id];
      if (mte == null || !workable.isInstance(mte)) continue;
      List<String> names = new ArrayList<>();
      try {
        for (Object map : (java.util.Collection<?>) workable.getMethod("getAvailableRecipeMaps").invoke(mte))
          if (map != null) names.add((String) map.getClass().getField("unlocalizedName").get(map));
      } catch (Throwable t) {}
      if (names.isEmpty()) {
        try {
          Object map = workable.getMethod("getRecipeMap").invoke(mte);
          if (map != null) names.add((String) map.getClass().getField("unlocalizedName").get(map));
        } catch (Throwable t) {}
      }
      if (names.isEmpty()) continue;
      int tier = -1;
      try { tier = ((Number) mte.getClass().getField("mTier").get(mte)).intValue(); } catch (Throwable t) {}
      if (m++ > 0) maps.append(",\n");
      maps.append("  \"").append(id).append("\":{\"tier\":").append(tier).append(",\"maps\":[");
      for (int i = 0; i < names.size(); i++) maps.append(i > 0 ? "," : "").append("\"").append(escape(names.get(i))).append("\"");
      maps.append("]}");
    }
    maps.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "gt_machine_maps.json")), StandardCharsets.UTF_8)) {
      wr.write(maps.toString());
    }
    System.out.println("[texturedump] wrote recipe maps of " + m + " GregTech machines");

    // texture-dump/recipe_maps.json: a GT recipe map's name in game ("gt.recipe.chemicalreactor" -> "Chemical Reactor")
    StringBuilder names = new StringBuilder("{\n");
    int k = 0;
    try {
      Map<?, ?> all = (Map<?, ?>) Class.forName("gregtech.api.recipe.RecipeMap").getField("ALL_RECIPE_MAPS").get(null);
      Class<?> stat = Class.forName("net.minecraft.util.StatCollector");
      for (Object map : all.values()) {
        String id = (String) map.getClass().getField("unlocalizedName").get(map);
        String shown = (String) callStatic(stat, "func_74838_a", "translateToLocal", id);
        if (shown == null || shown.equals(id)) continue;
        if (k++ > 0) names.append(",\n");
        names.append("  \"").append(escape(id)).append("\":\"").append(escape(shown)).append("\"");
      }
    } catch (Throwable t) {}
    names.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "recipe_maps.json")), StandardCharsets.UTF_8)) {
      wr.write(names.toString());
    }
    System.out.println("[texturedump] wrote " + k + " recipe map names");
  }

  // texture-dump/coke_oven.json: Railcraft coke oven recipes. a coke oven only saves how far it is, how long
  // its batch takes and what it gives depends on the recipe for its input
  void dumpCokeOven(File dir, Method nameOf, Object itemRegistry) throws Exception {
    List<?> recipes;
    try {
      Object manager = Class.forName("mods.railcraft.api.crafting.RailcraftCraftingManager").getField("cokeOven").get(null);
      recipes = (List<?>) manager.getClass().getMethod("getRecipes").invoke(manager);
    } catch (Throwable t) {
      System.out.println("[texturedump] no Railcraft coke oven, skipping coke_oven.json");
      return;
    }
    StringBuilder json = new StringBuilder("[\n");
    int n = 0;
    for (Object r : recipes) {
      try {
        Object in = r.getClass().getMethod("getInput").invoke(r), out = r.getClass().getMethod("getOutput").invoke(r);
        Object fluid = r.getClass().getMethod("getFluidOutput").invoke(r);
        int ticks = (Integer) r.getClass().getMethod("getCookTime").invoke(r);
        if (in == null) continue;
        StringBuilder e = new StringBuilder("  {");
        e.append("\"input\":").append(stackJson(in, nameOf, itemRegistry));
        if (out != null) e.append(",\"output\":").append(stackJson(out, nameOf, itemRegistry));
        if (fluid != null) {
          Object f = fluid.getClass().getMethod("getFluid").invoke(fluid);
          String fname = (String) f.getClass().getMethod("getName").invoke(f);
          int amount = fluid.getClass().getField("amount").getInt(fluid);
          e.append(",\"fluid\":\"").append(escape(fname)).append("\",\"fluid_mb\":").append(amount);
        }
        e.append(",\"cook_ticks\":").append(ticks).append("}");
        if (n++ > 0) json.append(",\n");
        json.append(e);
      } catch (Throwable t) {}
    }
    json.append("\n]\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "coke_oven.json")), StandardCharsets.UTF_8)) {
      wr.write(json.toString());
    }
    System.out.println("[texturedump] wrote " + n + " coke oven recipes");
  }

  // one recipe: what it's made in ("crafting", "smelting", or a GT recipe map's name), its EU/t for GT, its
  // ingredient slots (each a list of alternatives, ore dictionary entries give many) and outputs
  static class Recipe {
    String type;
    int eut;
    int ticks;  // how long a GregTech recipe runs, its energy is eut * ticks
    int w, h;  // a shaped crafting recipe's grid, its slots row by row with empty ones kept, 0 for shapeless
    final List<List<String>> in = new ArrayList<>();  // each alternative: "mod:name:damage", "fluid:name", damage 32767 is any
    final List<int[]> inMeta = new ArrayList<>();    // per slot: count (0 for not consumed: tools, GT circuits and molds), fluid mB
    final List<String> out = new ArrayList<>();
    final List<int[]> outMeta = new ArrayList<>();   // count or mB, chance in 1/10000
  }

  String key(Object stack, Method nameOf, Object itemRegistry) throws Exception {
    Object item = call(stack, "func_77973_b", "getItem");
    if (item == null) return null;
    return nameOf.invoke(itemRegistry, item) + ":" + call(stack, "func_77960_j", "getItemDamage");
  }

  // a crafting ingredient: a stack, a list of them (ore dictionary), or null (an empty slot)
  boolean addSlot(Recipe r, Object ing, Method nameOf, Object itemRegistry) throws Exception {
    if (ing == null) {
      // an empty cell of a shaped grid: kept so the slots still line up with the grid
      if (r.w > 0) {
        r.in.add(new ArrayList<>());
        r.inMeta.add(new int[] { 0 });
      }
      return true;
    }
    List<String> alts = new ArrayList<>();
    Object first = null;
    if (ing instanceof List) {
      for (Object o : (List<?>) ing) {
        if (o == null) continue;
        String k = key(o, nameOf, itemRegistry);
        if (k != null) { alts.add(k); if (first == null) first = o; }
      }
    } else {
      String k = key(ing, nameOf, itemRegistry);
      if (k != null) { alts.add(k); first = ing; }
    }
    // an ore dictionary entry nothing is registered for: the recipe can't be made
    if (alts.isEmpty()) return false;
    // HarvestCraft's cutting board, pot, juicer... come back from the crafting grid: a tool, not used up
    int count = 1;
    try {
      Object item = call(first, "func_77973_b", "getItem");
      Object back = item.getClass().getMethod("getContainerItem", Class.forName("net.minecraft.item.ItemStack")).invoke(item, first);
      if (back != null && call(back, "func_77973_b", "getItem") == item) count = 0;
    } catch (Throwable t) {}
    r.in.add(alts);
    r.inMeta.add(new int[] { count });
    return true;
  }

  // a GregTech input that's a circuit tier placeholder ("Any MV Circuit"): every item in its circuit... ore
  // dictionary entries, the placeholder first. anything else: just itself
  List<String> circuitAlternatives(Object stack, String k, Method nameOf, Object itemRegistry) {
    List<String> alts = new ArrayList<>();
    alts.add(k);
    try {
      Class<?> od = Class.forName("net.minecraftforge.oredict.OreDictionary");
      int[] ids = (int[]) od.getMethod("getOreIDs", Class.forName("net.minecraft.item.ItemStack")).invoke(null, stack);
      for (int id : ids) {
        String ore = (String) od.getMethod("getOreName", int.class).invoke(null, id);
        if (ore == null || !ore.startsWith("circuit")) continue;
        for (Object o : (List<?>) od.getMethod("getOres", String.class).invoke(null, ore)) {
          String ok = key(o, nameOf, itemRegistry);
          if (ok != null && !alts.contains(ok)) alts.add(ok);
        }
      }
    } catch (Throwable t) {}
    return alts;
  }

  // texture-dump/foods.json: every food (by AppleCore, which knows what Spice of Life counts) with its hunger
  // and saturation, and texture-dump/recipes.json: every crafting, smelting and GregTech recipe that leads to
  // a food, followed back from the foods through their ingredients
  void dumpFoods(File dir, Method nameOf, Object itemRegistry) throws Exception {
    Object accessor;
    Method isFood, values;
    try {
      Class<?> api = Class.forName("squeek.applecore.api.AppleCoreAPI");
      accessor = api.getField("accessor").get(null);
      Class<?> stackClass = Class.forName("net.minecraft.item.ItemStack");
      isFood = accessor.getClass().getMethod("isFood", stackClass);
      values = accessor.getClass().getMethod("getUnmodifiedFoodValues", stackClass);
    } catch (Throwable t) {
      System.out.println("[texturedump] no AppleCore, skipping foods.json");
      return;
    }
    StringBuilder foods = new StringBuilder("{\n");
    java.util.Set<String> foodKeys = new java.util.LinkedHashSet<>();
    for (Object stack : allStacks) {
      try {
        if (!(Boolean) isFood.invoke(accessor, stack)) continue;
        String k = key(stack, nameOf, itemRegistry);
        if (k == null || !foodKeys.add(k)) continue;
        Object v = values.invoke(accessor, stack);
        int hunger = v.getClass().getField("hunger").getInt(v);
        float sat = v.getClass().getField("saturationModifier").getFloat(v);
        String display = (String) call(stack, "func_82833_r", "getDisplayName");
        if (foodKeys.size() > 1) foods.append(",\n");
        foods.append("  \"").append(escape(k)).append("\":{\"name\":\"").append(escape(display)).append("\",\"hunger\":").append(hunger)
             .append(",\"saturation\":").append(sat).append("}");
      } catch (Throwable t) {}
    }
    foods.append("\n}\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(new File(dir, "foods.json")), StandardCharsets.UTF_8)) {
      wr.write(foods.toString());
    }
    System.out.println("[texturedump] wrote " + foodKeys.size() + " foods");

    List<Recipe> all = new ArrayList<>();
    int skipped = 0;
    // crafting table
    Object cm = callStatic(Class.forName("net.minecraft.item.crafting.CraftingManager"), "func_77594_a", "getInstance");
    Class<?> shaped = Class.forName("net.minecraft.item.crafting.ShapedRecipes"), shapeless = Class.forName("net.minecraft.item.crafting.ShapelessRecipes");
    for (Object ir : (List<?>) call(cm, "func_77592_b", "getRecipeList")) {
      try {
        Object out = call(ir, "func_77571_b", "getRecipeOutput");
        if (out == null) continue;
        Recipe r = new Recipe();
        r.type = "crafting";
        Iterable<?> ings = null;
        if (shaped.isInstance(ir)) {
          ings = java.util.Arrays.asList((Object[]) get(ir, "field_77574_d", "recipeItems"));
          r.w = (Integer) get(ir, "field_77576_b", "recipeWidth");
          r.h = (Integer) get(ir, "field_77577_c", "recipeHeight");
        }
        else if (shapeless.isInstance(ir)) ings = (List<?>) get(ir, "field_77579_b", "recipeItems");
        else {
          // forge's ore recipes and everything built on them (most of GregTech's and HarvestCraft's)
          Object in = ir.getClass().getMethod("getInput").invoke(ir);
          ings = in instanceof Object[] ? java.util.Arrays.asList((Object[]) in) : (List<?>) in;
          // forge's shaped ore recipe keeps its grid in private width and height
          if (in instanceof Object[]) {
            try {
              r.w = (Integer) field(ir.getClass(), "width", "width").get(ir);
              r.h = (Integer) field(ir.getClass(), "height", "height").get(ir);
              if (r.w * r.h != ((Object[]) in).length) r.w = r.h = 0;
            } catch (Throwable t) { r.w = r.h = 0; }
          }
        }
        boolean ok = true;
        for (Object ing : ings) ok &= addSlot(r, ing, nameOf, itemRegistry);
        if (!ok || r.in.isEmpty()) continue;
        r.out.add(key(out, nameOf, itemRegistry));
        r.outMeta.add(new int[] { (Integer) get(out, "field_77994_a", "stackSize"), 10000 });
        all.add(r);
      } catch (Throwable t) { skipped++; }
    }
    // furnace
    Object fr = callStatic(Class.forName("net.minecraft.item.crafting.FurnaceRecipes"), "func_77602_a", "smelting");
    for (Map.Entry<?, ?> e : ((Map<?, ?>) call(fr, "func_77599_b", "getSmeltingList")).entrySet()) {
      try {
        Recipe r = new Recipe();
        r.type = "smelting";
        if (!addSlot(r, e.getKey(), nameOf, itemRegistry)) continue;
        r.out.add(key(e.getValue(), nameOf, itemRegistry));
        r.outMeta.add(new int[] { (Integer) get(e.getValue(), "field_77994_a", "stackSize"), 10000 });
        all.add(r);
      } catch (Throwable t) { skipped++; }
    }
    // GregTech recipe maps
    try {
      Map<?, ?> maps = (Map<?, ?>) Class.forName("gregtech.api.recipe.RecipeMap").getField("ALL_RECIPE_MAPS").get(null);
      for (Object map : maps.values()) {
        String mapName = (String) map.getClass().getField("unlocalizedName").get(map);
        for (Object g : (java.util.Collection<?>) map.getClass().getMethod("getAllRecipes").invoke(map)) {
          try {
            Class<?> gc = g.getClass();
            if (gc.getField("mFakeRecipe").getBoolean(g) || !gc.getField("mEnabled").getBoolean(g)) continue;
            Recipe r = new Recipe();
            r.type = mapName;
            r.eut = gc.getField("mEUt").getInt(g);
            r.ticks = gc.getField("mDuration").getInt(g);
            boolean ok = true;
            for (Object st : (Object[]) gc.getField("mInputs").get(g)) {
              if (st == null) continue;
              String k = key(st, nameOf, itemRegistry);
              if (k == null) { ok = false; break; }
              // GregTech asks for "Any MV Circuit", a placeholder for the circuitGood ore dictionary entry: the
              // machine takes any circuit of that tier. so are all of those
              r.in.add(circuitAlternatives(st, k, nameOf, itemRegistry));
              r.inMeta.add(new int[] { (Integer) get(st, "field_77994_a", "stackSize") });
            }
            Object[] fin = (Object[]) gc.getField("mFluidInputs").get(g);
            for (int i = 0; fin != null && i < fin.length; i++) {
              if (fin[i] == null) continue;
              Object f = fin[i].getClass().getMethod("getFluid").invoke(fin[i]);
              r.in.add(java.util.Collections.singletonList("fluid:" + f.getClass().getMethod("getName").invoke(f)));
              r.inMeta.add(new int[] { fin[i].getClass().getField("amount").getInt(fin[i]) });
            }
            if (!ok || r.in.isEmpty()) continue;
            Object[] outs = (Object[]) gc.getField("mOutputs").get(g);
            int[] chances = (int[]) gc.getField("mChances").get(g);
            for (int i = 0; outs != null && i < outs.length; i++) {
              if (outs[i] == null) continue;
              String k = key(outs[i], nameOf, itemRegistry);
              if (k == null) continue;
              r.out.add(k);
              r.outMeta.add(new int[] { (Integer) get(outs[i], "field_77994_a", "stackSize"), chances != null && i < chances.length ? chances[i] : 10000 });
            }
            Object[] fout = (Object[]) gc.getField("mFluidOutputs").get(g);
            for (int i = 0; fout != null && i < fout.length; i++) {
              if (fout[i] == null) continue;
              Object f = fout[i].getClass().getMethod("getFluid").invoke(fout[i]);
              r.out.add("fluid:" + f.getClass().getMethod("getName").invoke(f));
              r.outMeta.add(new int[] { fout[i].getClass().getField("amount").getInt(fout[i]), 10000 });
            }
            if (!r.out.isEmpty()) all.add(r);
          } catch (Throwable t) { skipped++; }
        }
      }
    } catch (Throwable t) {
      System.out.println("[texturedump] no GregTech recipe maps");
    }

    // keep what leads to a food: start from the foods, take every recipe making something needed, need its
    // ingredients, repeat. an output matches a needed "name:damage" or a needed "name:32767" (any damage)
    Map<String, List<Recipe>> byOut = new HashMap<>();
    for (Recipe r : all)
      for (String o : r.out) {
        byOut.computeIfAbsent(o, x -> new ArrayList<>()).add(r);
        int c = o.lastIndexOf(':');
        if (!o.startsWith("fluid:")) byOut.computeIfAbsent(o.substring(0, c) + ":32767", x -> new ArrayList<>()).add(r);
      }
    java.util.Set<String> need = new java.util.HashSet<>(foodKeys);
    java.util.Set<Recipe> keep = new java.util.LinkedHashSet<>();
    java.util.ArrayDeque<String> work = new java.util.ArrayDeque<>(foodKeys);
    while (!work.isEmpty()) {
      List<Recipe> rs = byOut.get(work.poll());
      if (rs == null) continue;
      for (Recipe r : rs) {
        if (!keep.add(r)) continue;
        for (List<String> slot : r.in)
          for (String k : slot)
            if (need.add(k)) work.add(k);
      }
    }
    writeRecipes(new File(dir, "recipes.json"), keep);
    // everything, unfiltered, for looking things up (what makes ethanol?)
    writeRecipes(new File(dir, "all_recipes.json"), all);
    System.out.println("[texturedump] wrote " + keep.size() + " of " + all.size() + " recipes leading to food (" + need.size() + " items), " + skipped + " unreadable");
  }

  void writeRecipes(File file, Iterable<Recipe> recipes) throws Exception {
    StringBuilder json = new StringBuilder("[\n");
    int n = 0;
    for (Recipe r : recipes) {
      if (n++ > 0) json.append(",\n");
      json.append("{\"t\":\"").append(escape(r.type)).append("\"");
      if (r.eut != 0) json.append(",\"eut\":").append(r.eut);
      if (r.ticks != 0) json.append(",\"ticks\":").append(r.ticks);
      if (r.w > 0) json.append(",\"w\":").append(r.w).append(",\"h\":").append(r.h);
      json.append(",\"in\":[");
      for (int i = 0; i < r.in.size(); i++) {
        json.append(i > 0 ? "," : "").append("[").append(r.inMeta.get(i)[0]);
        for (String k : r.in.get(i)) json.append(",\"").append(escape(k)).append("\"");
        json.append("]");
      }
      json.append("],\"out\":[");
      for (int i = 0; i < r.out.size(); i++)
        json.append(i > 0 ? "," : "").append("[\"").append(escape(r.out.get(i))).append("\",").append(r.outMeta.get(i)[0]).append(",").append(r.outMeta.get(i)[1]).append("]");
      json.append("]}");
    }
    json.append("\n]\n");
    try (Writer wr = new OutputStreamWriter(new FileOutputStream(file), StandardCharsets.UTF_8)) {
      wr.write(json.toString());
    }
  }

  // {"item":"mod:name","damage":n,"count":n}, damage 32767 means any
  String stackJson(Object stack, Method nameOf, Object itemRegistry) throws Exception {
    Object item = call(stack, "func_77973_b", "getItem");
    String name = String.valueOf(nameOf.invoke(itemRegistry, item));
    int damage = (Integer) call(stack, "func_77960_j", "getItemDamage");
    int count = (Integer) get(stack, "field_77994_a", "stackSize");
    return "{\"item\":\"" + escape(name) + "\",\"damage\":" + damage + ",\"count\":" + count + "}";
  }

  static String escape(String s) {
    StringBuilder b = new StringBuilder();
    for (char c : s.toCharArray()) {
      if (c == '\\' || c == '"') b.append('\\').append(c);
      else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
      else b.append(c);
    }
    return b.toString();
  }

  // reflection, srg name first, then the deobfuscated one for dev environments

  static Method method(Class<?> c, String srg, String mcp, Class<?>... args) throws NoSuchMethodException {
    for (Class<?> k = c; k != null; k = k.getSuperclass()) {
      for (String n : new String[] { srg, mcp }) {
        try {
          Method m = k.getDeclaredMethod(n, args);
          m.setAccessible(true);
          return m;
        } catch (NoSuchMethodException e) {}
      }
    }
    // interface methods, like IIcon's
    for (String n : new String[] { srg, mcp }) {
      try { return c.getMethod(n, args); } catch (NoSuchMethodException e) {}
    }
    throw new NoSuchMethodException(c.getName() + "." + srg + "/" + mcp);
  }

  static Object call(Object target, String srg, String mcp, Object... args) throws Exception {
    return invoke(target.getClass(), target, srg, mcp, args);
  }

  static Object callStatic(Class<?> c, String srg, String mcp, Object... args) throws Exception {
    return invoke(c, null, srg, mcp, args);
  }

  static Object invoke(Class<?> c, Object target, String srg, String mcp, Object... args) throws Exception {
    Class<?>[] types = new Class<?>[args.length];
    for (int i = 0; i < args.length; i++) {
      Class<?> t = args[i].getClass();
      types[i] = t == Integer.class ? int.class : t;
    }
    // the location argument is declared as ResourceLocation, find it by name and arity instead
    try {
      return method(c, srg, mcp, types).invoke(target, args);
    } catch (NoSuchMethodException e) {
      for (Class<?> k = c; k != null; k = k.getSuperclass()) {
        for (Method m : k.getDeclaredMethods()) {
          if ((m.getName().equals(srg) || m.getName().equals(mcp)) && m.getParameterTypes().length == args.length) {
            m.setAccessible(true);
            return m.invoke(target, args);
          }
        }
      }
      throw e;
    }
  }

  static Object get(Object target, String srg, String mcp) throws Exception {
    return field(target.getClass(), srg, mcp).get(target);
  }

  static Object getStatic(Class<?> c, String srg, String mcp) throws Exception {
    return field(c, srg, mcp).get(null);
  }

  static Field field(Class<?> c, String srg, String mcp) throws NoSuchFieldException {
    for (Class<?> k = c; k != null; k = k.getSuperclass()) {
      for (String n : new String[] { srg, mcp }) {
        try {
          Field f = k.getDeclaredField(n);
          f.setAccessible(true);
          return f;
        } catch (NoSuchFieldException e) {}
      }
    }
    throw new NoSuchFieldException(c.getName() + "." + srg + "/" + mcp);
  }
}

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
  }

  static String escape(String s) {
    return s.replace("\\", "\\\\").replace("\"", "\\\"");
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

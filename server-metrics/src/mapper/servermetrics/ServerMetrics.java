// SPDX-License-Identifier: MPL-2.0
package mapper.servermetrics;

import cpw.mods.fml.common.FMLCommonHandler;
import cpw.mods.fml.common.Mod;
import cpw.mods.fml.common.event.FMLServerStartedEvent;
import cpw.mods.fml.common.event.FMLServerStoppingEvent;
import cpw.mods.fml.common.eventhandler.SubscribeEvent;
import cpw.mods.fml.common.gameevent.PlayerEvent;
import cpw.mods.fml.common.gameevent.TickEvent;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import javax.management.MBeanServer;
import javax.management.ObjectName;
import net.minecraftforge.common.MinecraftForge;
import net.minecraftforge.event.CommandEvent;
import net.minecraftforge.event.ServerChatEvent;
import net.minecraftforge.event.entity.EntityJoinWorldEvent;
import net.minecraftforge.event.entity.living.LivingDeathEvent;
import net.minecraftforge.event.terraingen.PopulateChunkEvent;
import net.minecraftforge.event.world.BlockEvent;
import net.minecraftforge.event.world.ChunkDataEvent;
import net.minecraftforge.event.world.ChunkEvent;
import net.minecraftforge.event.world.WorldEvent;

// server side numbers for prometheus: tick times, chunks, world gen, entities, tile entities, players and
// world activity, published as JMX beans under the "gtnh" domain. the prometheus jmx exporter agent that
// already runs in the server JVM exports them, its jmx.yaml turns them into minecraft_* metrics
@Mod(modid = "mapper-servermetrics", name = "Mapper Server Metrics", version = "1.0", acceptableRemoteVersions = "*")
public class ServerMetrics {
  static final String DOMAIN = "gtnh";
  final MBeanServer jmx = ManagementFactory.getPlatformMBeanServer();
  final Map<String, Bean> beans = new HashMap<>();

  // per dimension counters, events can come from the chunk io thread
  static class Counters {
    final AtomicLong chunkLoads = new AtomicLong(), chunkUnloads = new AtomicLong(), chunkDiskLoads = new AtomicLong(),
      chunkSaves = new AtomicLong(), chunksPopulated = new AtomicLong(), chunksGenerated = new AtomicLong(),
      populateNanos = new AtomicLong(), populateCpuNanos = new AtomicLong(), populateMaxNanos = new AtomicLong(),
      populateMismatches = new AtomicLong(), populateCpuUnavailable = new AtomicLong(), populateApproximate = new AtomicLong(), blocksBroken = new AtomicLong(),
      blocksPlaced = new AtomicLong(), entitySpawns = new AtomicLong(), deaths = new AtomicLong(), playerDeaths = new AtomicLong();
  }
  final Map<Integer, Counters> counters = new ConcurrentHashMap<>();
  final AtomicLong logins = new AtomicLong(), logouts = new AtomicLong(), chat = new AtomicLong(), commands = new AtomicLong(),
    worldLoads = new AtomicLong(), worldUnloads = new AtomicLong();
  // written on the server thread only, read by chunk events from the io thread too
  volatile int ticks = 0;

  Counters counters(int dim) { return counters.computeIfAbsent(dim, d -> new Counters()); }

  @Mod.EventHandler
  public void started(FMLServerStartedEvent e) {
    FMLCommonHandler.instance().bus().register(this);
    MinecraftForge.EVENT_BUS.register(this);
    System.out.println("[servermetrics] publishing JMX beans under " + DOMAIN);
  }

  @Mod.EventHandler
  public void stopping(FMLServerStoppingEvent e) {
    for (String name : new HashSet<>(beans.keySet())) unregister(name);
  }

  // counters

  // a chunk read from disk gets ChunkDataEvent.Load first, then ChunkEvent.Load. remembering which chunks
  // came from disk tells the generated ones apart exactly, per chunk
  // chunk -> the tick its data was read, see expireDiskLoads
  final Map<String, Integer> fromDisk = new ConcurrentHashMap<>();

  static String chunkKey(Object world, Object chunk) {
    return System.identityHashCode(world) + ":" + R.get(chunk, "field_76635_g", "xPosition") + ":" + R.get(chunk, "field_76647_h", "zPosition");
  }

  @SubscribeEvent public void chunkLoad(ChunkEvent.Load e) {
    Object w = world(e);
    Counters c = counters(dim(w));
    c.chunkLoads.incrementAndGet();
    if (fromDisk.remove(chunkKey(w, chunk(e))) == null) c.chunksGenerated.incrementAndGet();
  }
  @SubscribeEvent public void chunkUnload(ChunkEvent.Unload e) { counters(dim(world(e))).chunkUnloads.incrementAndGet(); }
  @SubscribeEvent public void chunkDiskLoad(ChunkDataEvent.Load e) {
    Object w = world(e);
    counters(dim(w)).chunkDiskLoads.incrementAndGet();
    fromDisk.put(chunkKey(w, chunk(e)), ticks);
  }
  @SubscribeEvent public void chunkSave(ChunkDataEvent.Save e) { counters(dim(world(e))).chunkSaves.incrementAndGet(); }
  // population timing: Pre and Post bracket it. populating a chunk can load and populate its neighbours
  // before its own Post, so frames stack up and each chunk is charged its time minus its nested children's.
  // wall time (what population takes out of the tick) and the thread's CPU time (what it burns) both
  static final java.lang.management.ThreadMXBean threads = ManagementFactory.getThreadMXBean();
  static final boolean cpuTime = enableCpuTime();

  static boolean enableCpuTime() {
    try {
      if (!threads.isCurrentThreadCpuTimeSupported()) return false;
      if (!threads.isThreadCpuTimeEnabled()) threads.setThreadCpuTimeEnabled(true);
      return true;
    } catch (Throwable t) {
      return false;
    }
  }

  // -1 when there's no CPU time: not supported, or switched off after start (getCurrentThreadCpuTime is -1 then)
  static long cpuNow() {
    if (!cpuTime) return -1;
    try {
      return threads.getCurrentThreadCpuTime();
    } catch (Throwable t) {
      return -1;
    }
  }

  static final class PopulateFrame {
    final Object world;
    final int x, z;
    final long start = System.nanoTime(), cpuStart = cpuNow();
    long children, childrenCpu;
    // false once any CPU reading for this frame or a nested one was missing, its CPU time can't be trusted then
    boolean cpuValid = cpuStart >= 0;
    // true once a stray was closed on top of this frame: the stray's end is only when it was noticed, so it
    // took some of this frame's own work with it and this frame's times are lower bounds
    boolean approximate;
    PopulateFrame(Object world, int x, int z) { this.world = world; this.x = x; this.z = z; }
  }
  final ThreadLocal<java.util.ArrayDeque<PopulateFrame>> populating = ThreadLocal.withInitial(java.util.ArrayDeque::new);

  @SubscribeEvent public void populateStart(PopulateChunkEvent.Pre e) {
    populating.get().push(new PopulateFrame(world(e), e.chunkX, e.chunkZ));
  }

  @SubscribeEvent public void populate(PopulateChunkEvent.Post e) {
    Object w = world(e);
    Counters c = counters(dim(w));
    c.chunksPopulated.incrementAndGet();
    java.util.ArrayDeque<PopulateFrame> stack = populating.get();
    // normally the top frame, but a generator that posts Pre without Post would leave strays above it.
    // a stray is closed as if its Post came now: its elapsed time goes to the frame below as a child, so the
    // matched frame's exclusive time doesn't absorb it. the stray's own time has no real end, so it isn't
    // counted anywhere, only the mismatch is (as is a Post without any Pre). the frame below is marked
    // approximate, the stray's made up end cut into its own time
    long now = System.nanoTime(), cpuEnd = cpuNow();
    PopulateFrame f = null;
    while (!stack.isEmpty()) {
      PopulateFrame top = stack.pop();
      if (top.world == w && top.x == e.chunkX && top.z == e.chunkZ) { f = top; break; }
      c.populateMismatches.incrementAndGet();
      PopulateFrame below = stack.peek();
      if (below != null) {
        below.approximate = true;
        below.children += now - top.start;
        if (top.cpuValid && cpuEnd >= 0) below.childrenCpu += cpuEnd - top.cpuStart;
        else below.cpuValid = false;
      }
    }
    if (f == null) {
      c.populateMismatches.incrementAndGet();
      return;
    }
    long total = now - f.start, own = total - f.children;
    long cpuTotal = f.cpuValid && cpuEnd >= 0 ? cpuEnd - f.cpuStart : -1;
    long cpuOwn = cpuTotal - f.childrenCpu;
    boolean cpuOk = cpuTotal >= 0 && cpuOwn >= 0;
    PopulateFrame parent = stack.peek();
    if (parent != null) {
      parent.children += total;
      // without this frame's CPU time the parent's "minus children" would be off, so it's out too
      if (cpuOk) parent.childrenCpu += cpuTotal;
      else parent.cpuValid = false;
    }
    // an approximate frame still counts, as a lower bound, and says so
    if (f.approximate) c.populateApproximate.incrementAndGet();
    c.populateNanos.addAndGet(own);
    c.populateMaxNanos.accumulateAndGet(own, Math::max);
    if (cpuOk) c.populateCpuNanos.addAndGet(cpuOwn);
    else if (cpuTime) c.populateCpuUnavailable.incrementAndGet();
  }
  @SubscribeEvent public void blockBreak(BlockEvent.BreakEvent e) { counters(dim(world(e))).blocksBroken.incrementAndGet(); }
  @SubscribeEvent public void blockPlace(BlockEvent.PlaceEvent e) { counters(dim(world(e))).blocksPlaced.incrementAndGet(); }
  @SubscribeEvent public void worldLoad(WorldEvent.Load e) { worldLoads.incrementAndGet(); }
  @SubscribeEvent public void worldUnload(WorldEvent.Unload e) { worldUnloads.incrementAndGet(); }
  @SubscribeEvent public void chat(ServerChatEvent e) { chat.incrementAndGet(); }
  @SubscribeEvent public void command(CommandEvent e) { commands.incrementAndGet(); }
  @SubscribeEvent public void login(PlayerEvent.PlayerLoggedInEvent e) { logins.incrementAndGet(); }
  @SubscribeEvent public void logout(PlayerEvent.PlayerLoggedOutEvent e) { logouts.incrementAndGet(); }

  @SubscribeEvent public void spawn(EntityJoinWorldEvent e) {
    if (isServerWorld(world(e))) counters(dim(world(e))).entitySpawns.incrementAndGet();
  }

  @SubscribeEvent public void death(LivingDeathEvent e) {
    Object entity = R.get(e, "entity", "entity");
    Object w = entity == null ? null : R.get(entity, "field_70170_p", "worldObj");
    if (!isServerWorld(w)) return;
    Counters c = counters(dim(w));
    c.deaths.incrementAndGet();
    if (R.is(entity, "net.minecraft.entity.player.EntityPlayer")) c.playerDeaths.incrementAndGet();
  }

  // sampling

  @SubscribeEvent
  public void tick(TickEvent.ServerTickEvent e) {
    if (e.phase != TickEvent.Phase.END || ++ticks % 20 != 0) return;
    try {
      sample(ticks % 100 == 0);
    } catch (Throwable t) {
      System.err.println("[servermetrics] sample failed: " + t);
    }
  }

  void sample(boolean breakdown) throws Exception {
    // the two events are back to back, a disk read still waiting 2 seconds later never got its ChunkEvent.Load
    // (a load that failed after reading), drop it so the map can't grow
    final int now = ticks;
    fromDisk.values().removeIf(t -> now - t > 40);
    Object server = R.callStatic("net.minecraft.server.MinecraftServer", "func_71276_C", "getServer");
    long[] times = (long[]) R.get(server, "field_71311_j", "tickTimeArray");
    Bean s = bean("type=Server");
    if (times != null) {
      double mean = meanMs(times), max = maxMs(times);
      s.set("tick_ms", mean);
      s.set("max_tick_ms", max);
      s.set("tps", Math.min(20.0, 1000.0 / Math.max(mean, 1e-3)));
    }
    s.set("players", ((Number) R.call(server, "func_71233_x", "getCurrentPlayerCount")).doubleValue());
    s.set("player_logins", logins.get());
    s.set("player_logouts", logouts.get());
    s.set("chat_messages", chat.get());
    s.set("commands", commands.get());
    s.set("world_loads", worldLoads.get());
    s.set("world_unloads", worldUnloads.get());
    s.set("uptime_ticks", ticks);

    @SuppressWarnings("unchecked")
    Map<Integer, long[]> worldTimes = (Map<Integer, long[]>) R.get(server, "worldTickTimes", "worldTickTimes");
    Class<?> dm = Class.forName("net.minecraftforge.common.DimensionManager");
    Integer[] ids = (Integer[]) dm.getMethod("getIDs").invoke(null);
    Set<String> live = new HashSet<>();
    live.add("type=Server");
    Map<String, Integer> playerDims = new HashMap<>(), pings = new HashMap<>();

    for (Integer id : ids) {
      Object w = dm.getMethod("getWorld", int.class).invoke(null, id);
      if (w == null) continue;
      String key = "type=Dimension,dim=" + id;
      live.add(key);
      Bean d = bean(key);
      long[] wt = worldTimes == null ? null : worldTimes.get(id);
      if (wt != null) {
        d.set("tick_ms", meanMs(wt));
        d.set("max_tick_ms", maxMs(wt));
      }
      List<?> entities = (List<?>) R.get(w, "field_72996_f", "loadedEntityList");
      List<?> tiles = (List<?>) R.get(w, "field_147482_g", "loadedTileEntityList");
      List<?> players = (List<?>) R.get(w, "field_73010_i", "playerEntities");
      Object provider = R.get(w, "field_73059_b", "theChunkProviderServer");
      List<?> chunks = provider == null ? null : (List<?>) R.get(provider, "field_73245_g", "loadedChunks");
      if (entities != null) d.set("entities", entities.size());
      if (tiles != null) d.set("tile_entities", tiles.size());
      if (players != null) d.set("players", players.size());
      if (chunks != null) d.set("loaded_chunks", chunks.size());
      try {
        Method persistent = Class.forName("net.minecraftforge.common.ForgeChunkManager")
          .getMethod("getPersistentChunksFor", Class.forName("net.minecraft.world.World"));
        d.set("forced_chunks", ((com.google.common.collect.Multimap<?, ?>) persistent.invoke(null, w)).keySet().size());
      } catch (Throwable t) {}
      Counters c = counters(id);
      d.set("chunk_loads", c.chunkLoads.get());
      d.set("chunk_unloads", c.chunkUnloads.get());
      d.set("chunk_disk_loads", c.chunkDiskLoads.get());
      d.set("chunks_generated", c.chunksGenerated.get());
      d.set("chunk_saves", c.chunkSaves.get());
      d.set("chunks_populated", c.chunksPopulated.get());
      d.set("populate_seconds", c.populateNanos.get() / 1e9);
      if (cpuTime) {
        d.set("populate_cpu_seconds", c.populateCpuNanos.get() / 1e9);
        d.set("populate_cpu_unavailable", c.populateCpuUnavailable.get());
      }
      d.set("populate_frame_mismatches", c.populateMismatches.get());
      d.set("populate_approximate", c.populateApproximate.get());
      // largest exclusive wall time of a single population since the last sample: the chunk's own work with its
      // nested populations subtracted, not end-to-end latency (an outer frame's latency includes its children)
      d.set("populate_max_seconds", c.populateMaxNanos.getAndSet(0) / 1e9);
      d.set("blocks_broken", c.blocksBroken.get());
      d.set("blocks_placed", c.blocksPlaced.get());
      d.set("entity_spawns", c.entitySpawns.get());
      d.set("deaths", c.deaths.get());
      d.set("player_deaths", c.playerDeaths.get());

      if (players != null) {
        for (Object p : players) {
          Object name = R.call(p, "func_70005_c_", "getCommandSenderName");
          Object ping = R.get(p, "field_71138_i", "ping");
          if (name == null) continue;
          playerDims.put(name.toString(), id);
          if (ping instanceof Number) pings.put(name.toString(), ((Number) ping).intValue());
        }
      }

      if (breakdown) {
        Map<String, Integer> byEntity = new HashMap<>(), byTile = new HashMap<>();
        if (entities != null)
          for (Object en : entities.toArray()) byEntity.merge(entityName(en), 1, Integer::sum);
        if (tiles != null)
          for (Object te : tiles.toArray()) byTile.merge(tileName(te), 1, Integer::sum);
        for (Map.Entry<String, Integer> en : byEntity.entrySet()) {
          String k = "type=Entity,dim=" + id + ",name=" + clean(en.getKey());
          bean(k).set("count", en.getValue());
          live.add(k);
        }
        for (Map.Entry<String, Integer> en : byTile.entrySet()) {
          String k = "type=TileEntity,dim=" + id + ",name=" + clean(en.getKey());
          bean(k).set("count", en.getValue());
          live.add(k);
        }
      }
    }

    for (Map.Entry<String, Integer> p : playerDims.entrySet()) {
      String k = "type=Player,name=" + clean(p.getKey());
      Bean b = bean(k);
      b.set("dimension", p.getValue());
      if (pings.containsKey(p.getKey())) b.set("ping_ms", pings.get(p.getKey()));
      live.add(k);
    }

    // the breakdown only refreshes every 5 seconds, keep its beans in between
    for (String name : new HashSet<>(beans.keySet())) {
      boolean typed = name.startsWith("type=Entity,") || name.startsWith("type=TileEntity,");
      if (!live.contains(name) && (breakdown || !typed)) unregister(name);
    }
  }

  // helpers

  static final String[] SERVER = { "tick_ms", "max_tick_ms", "tps", "players", "player_logins", "player_logouts",
    "chat_messages", "commands", "world_loads", "world_unloads", "uptime_ticks" };
  static final String[] DIMENSION = { "tick_ms", "max_tick_ms", "entities", "tile_entities", "players", "loaded_chunks",
    "forced_chunks", "chunk_loads", "chunk_unloads", "chunk_disk_loads", "chunks_generated", "chunk_saves",
    "chunks_populated", "populate_seconds", "populate_cpu_seconds", "populate_max_seconds",
    "populate_cpu_unavailable", "populate_frame_mismatches", "populate_approximate", "blocks_broken", "blocks_placed", "entity_spawns", "deaths", "player_deaths" };
  static final String[] COUNT = { "count" };
  static final String[] PLAYER = { "dimension", "ping_ms" };

  static String[] attributesFor(String key) {
    if (key.startsWith("type=Server")) return SERVER;
    if (key.startsWith("type=Dimension,")) return DIMENSION;
    if (key.startsWith("type=Player,")) return PLAYER;
    return COUNT;
  }

  Bean bean(String key) throws Exception {
    Bean b = beans.get(key);
    if (b == null) {
      b = new Bean(attributesFor(key));
      jmx.registerMBean(b, new ObjectName(DOMAIN + ":" + key));
      beans.put(key, b);
    }
    return b;
  }

  void unregister(String key) {
    beans.remove(key);
    try { jmx.unregisterMBean(new ObjectName(DOMAIN + ":" + key)); } catch (Exception e) {}
  }

  // object name values can't hold , = : " * ? or newlines
  static String clean(String s) {
    return s.replaceAll("[,=:\"*?\\n\\\\]", ".");
  }

  static double meanMs(long[] ns) {
    long sum = 0;
    for (long v : ns) sum += v;
    return sum / (double) ns.length / 1e6;
  }

  static double maxMs(long[] ns) {
    long max = 0;
    for (long v : ns) max = Math.max(max, v);
    return max / 1e6;
  }

  static Object world(Object event) { return R.get(event, "world", "world"); }

  // ChunkEvent keeps its chunk private, behind getChunk()
  static Object chunk(Object event) { return R.call(event, "getChunk", "getChunk"); }

  static int dim(Object world) {
    Object provider = world == null ? null : R.get(world, "field_73011_w", "provider");
    Object id = provider == null ? null : R.get(provider, "field_76574_g", "dimensionId");
    return id instanceof Integer ? (Integer) id : 0;
  }

  // EntityJoinWorld and deaths also fire on clients, only count the server's worlds
  static boolean isServerWorld(Object world) { return R.is(world, "net.minecraft.world.WorldServer"); }

  static String entityName(Object entity) {
    if (R.is(entity, "net.minecraft.entity.player.EntityPlayer")) return "Player";
    Object n = R.callStatic1("net.minecraft.entity.EntityList", "func_75621_b", "getEntityString", "net.minecraft.entity.Entity", entity);
    return n != null ? n.toString() : entity.getClass().getSimpleName();
  }

  static Map<Class<?>, String> tileNames;

  @SuppressWarnings("unchecked")
  static String tileName(Object te) {
    if (tileNames == null) {
      Object m = R.getStatic("net.minecraft.tileentity.TileEntity", "field_145853_j", "classToNameMap");
      tileNames = m instanceof Map ? (Map<Class<?>, String>) m : new HashMap<>();
    }
    String n = tileNames.get(te.getClass());
    return n != null ? n : te.getClass().getSimpleName();
  }
}

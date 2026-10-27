// SPDX-License-Identifier: MPL-2.0
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "utils.h"
#include "metrics_http.h"
#include "nbt.h"
#include "nbt_tree.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <map>
#include <mutex>
#include <tuple>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

using namespace std;
using namespace filesystem;
using json = nlohmann::json;

template <typename F> void for_each_file_matching_pattern(const path dir, const regex pattern, F func) {
  filesystem::directory_iterator i{dir};
  smatch matches;
  for (auto &ent : i) {
    if (ent.is_regular_file()) {
      const auto &name = ent.path().stem().string();
      if (regex_search(name, matches, pattern)) {
        func(ent.path(), matches);
      }
    }
  }
}

// GTNH stores pollution in the `dim/gregtech/Pollution.X.Z.dat` files
// the first 3 bytes are a header, containing the magic# (always 0), the version (0), and if the first element is default or not
// it then stores alternating sets of "default value" and "non default value"
// a "default value" is just a 16bit number, saying the next N entries are the default (0 pollution)
// a "non default value" is a 16bit count, and then N 32bit numbers, for the pollution ammounts
// the file contains a total of 4096 entries, which map to a 64x64 chunk grid
template <typename F> void parse_pollution_file(int x, int z, const path p, F func) {
  const string data = readFile(p);
  if (data.size() < 3) throw runtime_error("file too short");
  const uint8_t *raw = (const uint8_t*)data.data();
  uint8_t nullRange = raw[2];
  size_t ptr = 3;
  uint32_t map[4096] = {};
  int ptr2 = 0;
  while (ptr + 2 <= data.size()) {
    uint16_t length = ntohs(*(uint16_t*)(raw + ptr));
    ptr += 2;
    if (ptr2 + length > 4096) throw runtime_error("too many entries");
    if (nullRange) {
      ptr2 += length;
    } else {
      if (ptr + length * 4 > data.size()) throw runtime_error("truncated");
      for (int i=0; i<length; i++) {
        map[ptr2++] = ntohl(*(uint32_t*)(raw + ptr));
        ptr += 4;
      }
    }
    nullRange = !nullRange;
  }
  if (ptr2 != 4096) throw runtime_error("expected 4096 entries, got " + to_string(ptr2));
  for (int i=0; i<4096; i++) {
    if (map[i] != 0) {
      func((i / 64) + (x * 64), (i % 64) + (z * 64), map[i]);
    }
  }
}

// GT5u default thresholds (gregtech.cfg, Pollution section)
static const struct { const char *name; uint32_t limit; } pollution_thresholds[] = {
  {"smog", 500000},
  {"poison", 750000},
  {"vegetation", 1000000},
  {"sour_rain", 2000000},
};

struct DimPollution {
  uint64_t total = 0;
  uint32_t max = 0;
  uint64_t chunks = 0;
  uint64_t over[4] = {};
};

static void fmt(string &out, const char *f, ...) __attribute__((format(printf, 2, 3)));
static void fmt(string &out, const char *f, ...) {
  char line[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(line, sizeof(line), f, ap);
  va_end(ap);
  out += line;
}

// prometheus label values must escape \ " and newlines
static string esc(const string &in) {
  string o;
  for (char c : in) {
    if (c == '\\' || c == '"') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

void collect_pollution(const path savepath, string &out) {
  string chunks;
  map<int, DimPollution> dims;
  uint64_t errors = 0;
  for_each_dimension(savepath, [&](path directory, int dimension) -> void {
    directory /= "gregtech";
    if (!exists(directory)) return;
    DimPollution &d = dims[dimension];
    for_each_file_matching_pattern(directory, regex("^Pollution\\.([-0-9]+)\\.([-0-9]+)$"), [&](path p, smatch matches) -> void {
      int x = atoi(matches[1].str().c_str());
      int z = atoi(matches[2].str().c_str());
      try {
        parse_pollution_file(x, z, p, [&](int cx, int cz, uint32_t v) {
          fmt(chunks, "chunk_pollution{dim=\"%d\",x=\"%d\",z=\"%d\"} %u\n", dimension, cx, cz, v);
          d.total += v;
          d.max = max(d.max, v);
          d.chunks++;
          for (int t = 0; t < 4; t++)
            if (v >= pollution_thresholds[t].limit) d.over[t]++;
        });
      } catch (const exception &e) {
        errors++;
        fprintf(stderr, "failed to parse %s: %s\n", p.c_str(), e.what());
      }
    });
  });

  out += "# HELP chunk_pollution GregTech pollution per chunk\n# TYPE chunk_pollution gauge\n";
  out += chunks;
  out += "# HELP pollution_total Sum of pollution over all chunks in a dimension\n# TYPE pollution_total gauge\n";
  for (auto &[dim, d] : dims) fmt(out, "pollution_total{dim=\"%d\"} %lu\n", dim, d.total);
  out += "# HELP pollution_max Highest chunk pollution in a dimension\n# TYPE pollution_max gauge\n";
  for (auto &[dim, d] : dims) fmt(out, "pollution_max{dim=\"%d\"} %u\n", dim, d.max);
  out += "# HELP pollution_polluted_chunks Chunks with non-zero pollution\n# TYPE pollution_polluted_chunks gauge\n";
  for (auto &[dim, d] : dims) fmt(out, "pollution_polluted_chunks{dim=\"%d\"} %lu\n", dim, d.chunks);
  out += "# HELP pollution_chunks_over_threshold Chunks at or above a GT pollution effect threshold (default config values)\n# TYPE pollution_chunks_over_threshold gauge\n";
  for (auto &[dim, d] : dims)
    for (int t = 0; t < 4; t++)
      fmt(out, "pollution_chunks_over_threshold{dim=\"%d\",effect=\"%s\",threshold=\"%u\"} %lu\n",
          dim, pollution_thresholds[t].name, pollution_thresholds[t].limit, d.over[t]);
  out += "# HELP pollution_parse_errors Pollution files that failed to parse on this scrape\n# TYPE pollution_parse_errors gauge\n";
  fmt(out, "pollution_parse_errors %lu\n", errors);
}

// world wide state from level.dat
void collect_world(const path savepath, string &out) {
  NbtTag level;
  try {
    level = loadGzipNbt(savepath / "level.dat");
  } catch (const exception &e) {
    fprintf(stderr, "failed to read level.dat: %s\n", e.what());
    return;
  }
  const NbtTag *data = level.get("Data");
  if (!data) return;
  const NbtTag *name = data->get("LevelName");
  string lname = esc(name ? name->s : "");

  struct { const char *metric, *key, *help; double scale; } fields[] = {
    {"world_time_ticks", "Time", "Total world ticks", 1},
    {"world_daytime_ticks", "DayTime", "Day time ticks, including skipped nights", 1},
    {"world_raining", "raining", "1 if raining", 1},
    {"world_thundering", "thundering", "1 if thundering", 1},
    {"world_rain_time_ticks", "rainTime", "Ticks until rain toggles", 1},
    {"world_thunder_time_ticks", "thunderTime", "Ticks until thunder toggles", 1},
    {"world_last_played_seconds", "LastPlayed", "Unix time of the last level.dat save", 0.001},
    {"world_spawn_x", "SpawnX", "World spawn X", 1},
    {"world_spawn_y", "SpawnY", "World spawn Y", 1},
    {"world_spawn_z", "SpawnZ", "World spawn Z", 1},
  };
  for (auto &f : fields) {
    if (!data->get(f.key)) continue;
    fmt(out, "# HELP %s %s\n# TYPE %s gauge\n%s{world=\"%s\"} %.17g\n",
        f.metric, f.help, f.metric, f.metric, lname.c_str(), data->num(f.key) * f.scale);
  }
  fmt(out, "# HELP world_day Current in-game day\n# TYPE world_day gauge\nworld_day{world=\"%s\"} %ld\n",
      lname.c_str(), (int64_t)data->num("DayTime") / 24000);
}

// uuid -> name, from the server's usercache.json (next to the world dir)
static map<string, string> load_usercache(const path savepath) {
  map<string, string> names;
  for (path p : {savepath / "usercache.json", savepath.parent_path() / "usercache.json"}) {
    if (!exists(p)) continue;
    try {
      for (auto &e : json::parse(readFile(p)))
        names[e.value("uuid", "")] = e.value("name", "");
    } catch (const exception &e) {
      fprintf(stderr, "failed to parse %s: %s\n", p.c_str(), e.what());
    }
    break;
  }
  return names;
}

void collect_players(const path savepath, string &out) {
  const auto names = load_usercache(savepath);
  auto labels = [&](const string &uuid) {
    auto it = names.find(uuid);
    return "uuid=\"" + esc(uuid) + "\",name=\"" + esc(it == names.end() ? "" : it->second) + "\"";
  };

  // playerdata: live state as of the last save
  map<string, string> series;  // metric -> lines
  auto add = [&](const char *metric, const string &l, double v) {
    fmt(series[metric], "%s{%s} %.17g\n", metric, l.c_str(), v);
  };
  path pd = savepath / "playerdata";
  if (exists(pd)) {
    for (auto &ent : directory_iterator(pd)) {
      if (ent.path().extension() != ".dat") continue;
      const string uuid = ent.path().stem().string();
      NbtTag p;
      try {
        p = loadGzipNbt(ent.path());
      } catch (const exception &e) {
        fprintf(stderr, "failed to read %s: %s\n", ent.path().c_str(), e.what());
        continue;
      }
      string l = labels(uuid);
      const NbtTag *pos = p.get("Pos");
      if (pos && pos->list.size() == 3) {
        string pl = l + ",dim=\"" + to_string((int)p.num("Dimension")) + "\"";
        add("player_x", pl, pos->list[0].d);
        add("player_y", pl, pos->list[1].d);
        add("player_z", pl, pos->list[2].d);
      }
      add("player_dimension", l, p.num("Dimension"));
      // 1.7.10 stores health as both a short and a more precise float
      add("player_health", l, p.get("HealF") ? p.num("HealF") : p.num("Health"));
      add("player_food_level", l, p.num("foodLevel"));
      add("player_xp_level", l, p.num("XpLevel"));
      add("player_xp_total", l, p.num("XpTotal"));
      add("player_score", l, p.num("Score"));
      auto mtime = chrono::file_clock::to_sys(last_write_time(ent.path()));
      add("player_last_save_seconds", l, chrono::duration<double>(mtime.time_since_epoch()).count());
    }
  }
  const map<string, string> help = {
    {"player_x", "Player X position"}, {"player_y", "Player Y position"}, {"player_z", "Player Z position"},
    {"player_dimension", "Dimension the player is in"}, {"player_health", "Player health (20 = full)"},
    {"player_food_level", "Player hunger (20 = full)"}, {"player_xp_level", "Player XP level"},
    {"player_xp_total", "Player total XP"}, {"player_score", "Player score"},
    {"player_last_save_seconds", "Unix time the playerdata file was last written"},
  };
  for (auto &[metric, lines] : series) {
    fmt(out, "# HELP %s %s\n# TYPE %s gauge\n", metric.c_str(), help.at(metric).c_str(), metric.c_str());
    out += lines;
  }

  // stats/<uuid>.json: vanilla statistics. per-item stats are summed to keep cardinality sane
  path sd = savepath / "stats";
  if (!exists(sd)) return;
  string stat_lines, sum_lines;
  for (auto &ent : directory_iterator(sd)) {
    if (ent.path().extension() != ".json") continue;
    const string l = labels(ent.path().stem().string());
    json j;
    try {
      j = json::parse(readFile(ent.path()));
    } catch (const exception &e) {
      fprintf(stderr, "failed to parse %s: %s\n", ent.path().c_str(), e.what());
      continue;
    }
    map<string, double> sums;
    for (auto &[key, val] : j.items()) {
      if (!val.is_number() || key.rfind("stat.", 0) != 0) continue;
      string rest = key.substr(5);
      size_t dot = rest.find('.');
      if (dot != string::npos) {
        // stat.mineBlock.<id>, stat.craftItem.<id>, stat.useItem.<id>, ...
        sums[rest.substr(0, dot)] += val.get<double>();
      } else {
        fmt(stat_lines, "player_stat{%s,stat=\"%s\"} %.17g\n", l.c_str(), esc(rest).c_str(), val.get<double>());
      }
    }
    for (auto &[cat, v] : sums)
      fmt(sum_lines, "player_stat_items_total{%s,stat=\"%s\"} %.17g\n", l.c_str(), esc(cat).c_str(), v);
  }
  out += "# HELP player_stat Vanilla player statistic (stats/<uuid>.json), e.g. deaths, mobKills, playOneMinute (ticks), walkOneCm\n# TYPE player_stat gauge\n";
  out += stat_lines;
  out += "# HELP player_stat_items_total Per-item statistics summed over all items, e.g. mineBlock, craftItem, useItem\n# TYPE player_stat_items_total gauge\n";
  out += sum_lines;
}

// containers
//
// item stacks in tile entity inventories: any list right under the tile entity whose entries carry an item
// (chests' "Items", GregTech's "Inventory", Railcraft's "inv" and "invStructure"...: id, Damage, Count),
// Storage Drawers ("Slots": Item, Meta, Count) and Draconic Evolution's placed items (one compound per
// stack, "Item0", "Item1"...: id, Damage, Count). only chunks claimed with ServerUtilities count,
// so dungeon and village loot stays out, summed per team, dimension and item. a region file is only parsed
// again when it changed on disk

// texture-dump's items.json: registry name -> damage -> display name. without it items are named "mod:item:damage"
static map<string, map<int, string>> item_names;

struct ItemKey {
  int id, damage;
  bool operator<(const ItemKey &o) const { return id != o.id ? id < o.id : damage < o.damage; }
};

struct Boiler {
  int x, y, z, tanks;
  string pressure;
  double heat, max_heat, steam_per_tick;
  bool burning;
  int64_t water, steam;
  map<ItemKey, int64_t> fuel_items;  // the firebox inventory: solid fuel waiting, a liquid firebox's containers
  map<string, int64_t> fuel_fluids;  // a liquid firebox's fuel tank (creosote, oil...)
};

// GregTech single block boilers, from GT 5.09.51's MetaTileEntityIDs and MTEBoiler: every 10 ticks, while
// mTemperature > 100 and there is water, a boiler makes half its getProductionPerSecond(), so above 100 it runs
// at its full rate. solar boilers' output depends on config, calcification and sunlight, none of which is in the
// save, so they get no production figure
struct GtBoilerSpec { int id; const char *name; int steam_per_second; int max_temperature; };
static const GtBoilerSpec gt_boilers[] = {
  { 100, "Small Coal Boiler", 120, 500 },
  { 101, "High Pressure Coal Boiler", 300, 1000 },
  { 102, "High Pressure Lava Boiler", 600, 1000 },
  { 105, "Simple Solar Boiler", -1, 500 },
  { 114, "High Pressure Solar Boiler", -1, 1000 },
};

struct GtBoiler {
  int x, y, z;
  string name;
  double temperature, max_temperature, steam_per_second;  // NaN steam_per_second: can't be worked out (solar)
  bool active;
  int64_t water, steam;
  map<ItemKey, int64_t> fuel_items;  // its inventory: fuel waiting, and ash
};

struct ChunkContainers {
  map<ItemKey, int64_t> items;
  map<string, int64_t> tiles;  // tile entity id -> how many held items
  map<pair<string, string>, int64_t> fluids;  // (fluid, tile entity id) -> mB
  vector<Boiler> boilers;
  vector<GtBoiler> gt_boilers;
};

struct RegionContainers {
  int64_t mtime = 0, size = -1;
  map<int, ChunkContainers> chunks;  // by index in the region, x + z * 32
};

static mutex container_lock;
static map<string, RegionContainers> container_cache;

struct StackParse {
  int id = -1, damage = 0;
  int64_t count = 0;
};

struct FluidParse {
  string name;
  int64_t amount = 0;
};

struct TileParse {
  string id;
  map<string, StackParse> stacks;  // by "<list>/<i>" or "Item<i>"
  // any compound holding FluidName and Amount, at any depth: Railcraft "tanks/<i>", GT "mFluid", a certus
  // quartz tank's root, TConstruct "Liquids/<i>"... keyed by the compound's path inside the tile entity
  map<string, FluidParse> fluids;
  map<string, double> numbers;  // the tile entity's own top level numbers (heat, pattern, master, x...)
};

struct ChunkParse {
  map<int, TileParse> tiles;
};

// "/Level/TileEntities/<n>/<list>/<i>/<field>" or ".../Item<i>/<field>", the stack and its field. only fields
// directly in the entry count, an item's own NBT ("<list>/<i>/tag/...", a backpack's contents) doesn't
static StackParse *stack_field(ChunkParse *c, const string &key, const char *&field) {
  static const char prefix[] = "/Level/TileEntities/";
  if (key.compare(0, sizeof(prefix) - 1, prefix) != 0) return nullptr;
  char *end;
  int te = strtol(key.c_str() + sizeof(prefix) - 1, &end, 10);
  if (*end != '/') return nullptr;
  const char *name = end + 1;
  const char *slash = strchr(name, '/');
  if (!slash) return nullptr;
  string stack;
  if (!strncmp(name, "Item", 4) && isdigit((unsigned char)name[4])) {
    // Item<i>/<field>
    for (const char *d = name + 4; d < slash; d++) if (!isdigit((unsigned char)*d)) return nullptr;
    stack.assign(name, slash - name);
    field = slash + 1;
  } else {
    // <list>/<i>/<field>
    const char *idx = slash + 1;
    char *after;
    strtol(idx, &after, 10);
    if (after == idx || *after != '/') return nullptr;
    stack.assign(name, after - name);
    field = after + 1;
  }
  if (strchr(field, '/')) return nullptr;
  return &c->tiles[te].stacks[stack];
}

// "/Level/TileEntities/<n>/<rest>" -> the tile entity and <rest>
static TileParse *tile_field(ChunkParse *c, const string &key, const char *&rest) {
  static const char prefix[] = "/Level/TileEntities/";
  if (key.compare(0, sizeof(prefix) - 1, prefix) != 0) return nullptr;
  char *end;
  int te = strtol(key.c_str() + sizeof(prefix) - 1, &end, 10);
  if (*end != '/') return nullptr;
  rest = end + 1;
  return &c->tiles[te];
}

// a FluidName or Amount: the compound around it is a fluid stack
static FluidParse *fluid_field(TileParse *te, const char *rest, const char *field) {
  const char *slash = strrchr(rest, '/');
  const char *last = slash ? slash + 1 : rest;
  if (strcmp(last, field) != 0) return nullptr;
  return &te->fluids[string(rest, slash ? slash - rest : 0)];
}

static void tile_number(const nbt_callbacks *cb, const string &key, double v) {
  const char *rest;
  TileParse *te = tile_field((ChunkParse *)cb->state, key, rest);
  if (!te) return;
  if (!strchr(rest, '/')) te->numbers[rest] = v;
  if (FluidParse *f = fluid_field(te, rest, "Amount")) f->amount = (int64_t)v;
}

static void tagf_tile(const nbt_callbacks *cb, const string key, double v) { tile_number(cb, key, v); }

static void stack_number(const nbt_callbacks *cb, const string &key, int64_t v) {
  tile_number(cb, key, (double)v);
  const char *field;
  StackParse *st = stack_field((ChunkParse *)cb->state, key, field);
  if (!st) return;
  if (!strcmp(field, "id") || !strcmp(field, "Item")) st->id = v;
  else if (!strcmp(field, "Damage") || !strcmp(field, "Meta")) st->damage = v;
  else if (!strcmp(field, "Count")) st->count = v;
}

static void tag1_stack(const nbt_callbacks *cb, const string key, uint8_t v) { stack_number(cb, key, (int8_t)v); }
static void tag2_stack(const nbt_callbacks *cb, const string key, uint16_t v) { stack_number(cb, key, (int16_t)v); }
static void tag3_stack(const nbt_callbacks *cb, const string key, int32_t v) { stack_number(cb, key, v); }
static void tag8_tile(const nbt_callbacks *cb, const string key, const string v) {
  const char *rest;
  TileParse *te = tile_field((ChunkParse *)cb->state, key, rest);
  if (!te) return;
  if (!strcmp(rest, "id")) te->id = v;
  if (FluidParse *f = fluid_field(te, rest, "FluidName")) f->name = v;
}

// Railcraft boilers, from Railcraft 9.16's TileBoiler.patterns (the "pattern" a boiler saves indexes it) and
// SteamBoiler.convertSteam: every ticksPerCycle ticks, while heat >= 100, numTanks * heat / 500 water turns
// into 160 times as much steam, as far as the water lasts
static const struct { int width, height, ticks_per_cycle; const char *pressure; } railcraft_boilers[] = {
  { 3, 4, 8, "high" }, { 3, 3, 8, "high" }, { 3, 2, 8, "high" }, { 2, 3, 8, "high" }, { 2, 2, 8, "high" }, { 1, 1, 8, "high" },
  { 3, 4, 16, "low" }, { 3, 3, 16, "low" }, { 3, 2, 16, "low" }, { 2, 3, 16, "low" }, { 2, 2, 16, "low" }, { 1, 1, 16, "low" },
};

static void scan_region(const path &p, RegionContainers &r) {
  string data = readFile(p);
  if (data.size() < 8192) return;
  const uint8_t *d = (const uint8_t *)data.data();
  vector<uint8_t> buf(4 << 20);
  for (int i = 0; i < 1024; i++) {
    uint32_t loc = ntohl(*(const uint32_t *)(d + i * 4));
    size_t off = (size_t)(loc >> 8) * 4096;
    if (!off || off + 5 > data.size()) continue;
    uint32_t len = ntohl(*(const uint32_t *)(d + off));
    if (len < 2 || off + 4 + len > data.size() || d[off + 4] != 2) continue;  // 2 = zlib
    int got;
    while ((got = decompress(d + off + 5, len - 1, buf.data(), buf.size())) < 0 && buf.size() < (64u << 20)) buf.resize(buf.size() * 2);
    if (got <= 0) continue;

    ChunkParse chunk;
    nbt_callbacks cb = {};
    cb.state = &chunk;
    cb.tag1 = tag1_stack;
    cb.tag2 = tag2_stack;
    cb.tag3 = tag3_stack;
    cb.tag8 = tag8_tile;
    cb.tagf = tagf_tile;
    parseNBT(buf.data(), got, &cb, false);
    for (auto &[n, te] : chunk.tiles) {
      bool any = false;
      for (auto &[slot, st] : te.stacks) {
        if (st.id <= 0 || st.count <= 0) continue;
        r.chunks[i].items[{st.id, st.damage}] += st.count;
        any = true;
      }
      const string type = te.id.empty() ? "unknown" : te.id;
      for (auto &[where, f] : te.fluids) {
        if (f.name.empty() || f.amount <= 0) continue;
        r.chunks[i].fluids[{f.name, type}] += f.amount;
        any = true;
      }
      if (any) r.chunks[i].tiles[type]++;

      if (te.id == "BaseMetaTileEntity") {
        int id = (int)te.numbers["mID"];
        for (auto &spec : gt_boilers) {
          if (spec.id != id) continue;
          GtBoiler b;
          b.x = (int)te.numbers["x"]; b.y = (int)te.numbers["y"]; b.z = (int)te.numbers["z"];
          b.name = spec.name;
          b.temperature = te.numbers["mTemperature"];
          b.max_temperature = spec.max_temperature;
          b.active = te.numbers["mActive"] != 0;
          b.water = te.fluids.count("mFluid") && te.fluids["mFluid"].name == "water" ? te.fluids["mFluid"].amount : 0;
          b.steam = te.fluids.count("mSteam") ? te.fluids["mSteam"].amount : 0;
          for (auto &[slot, st] : te.stacks)
            if (st.id > 0 && st.count > 0) b.fuel_items[{st.id, st.damage}] += st.count;
          if (spec.steam_per_second < 0) b.steam_per_second = NAN;
          else b.steam_per_second = b.temperature > 100 && b.water > 0 ? spec.steam_per_second : 0;
          r.chunks[i].gt_boilers.push_back(b);
        }
      }

      // the firebox of a formed boiler's master block holds the whole boiler's state
      if ((te.id == "RCBoilerFireboxSoildTile" || te.id == "RCBoilerFireboxLiquidTile") && te.numbers["master"] == 1) {
        int pattern = (int)te.numbers["pattern"];
        if (pattern < 0 || pattern >= (int)size(railcraft_boilers)) continue;
        auto &spec = railcraft_boilers[pattern];
        Boiler b;
        b.x = (int)te.numbers["x"]; b.y = (int)te.numbers["y"]; b.z = (int)te.numbers["z"];
        b.tanks = spec.width * spec.width * spec.height;
        b.pressure = spec.pressure;
        b.heat = te.numbers["heat"];
        b.max_heat = te.numbers["maxHeat"];
        b.burning = te.numbers["burnTime"] > 0;
        b.water = b.steam = 0;
        for (auto &[where, f] : te.fluids) {
          if (f.name == "water") b.water += f.amount;
          else if (f.name == "steam") b.steam += f.amount;
          else if (!f.name.empty() && f.amount > 0) b.fuel_fluids[f.name] += f.amount;
        }
        for (auto &[slot, st] : te.stacks)
          if (st.id > 0 && st.count > 0) b.fuel_items[{st.id, st.damage}] += st.count;
        double water_per_cycle = b.heat >= 100 ? b.tanks * b.heat / 500.0 : 0;
        b.steam_per_tick = 160.0 * min(water_per_cycle, (double)b.water) / spec.ticks_per_cycle;
        r.chunks[i].boilers.push_back(b);
      }
    }
  }
}

// ServerUtilities claims: teams/<id>.dat names the team, teams/claimedchunks/<id>.dat lists its chunks per dimension
struct Claims {
  map<tuple<int, int, int>, string> owner;  // (dim, chunk x, chunk z) -> team
  map<pair<string, int>, int64_t> claimed, loaded;  // (team, dim) -> chunks
};

static Claims load_claims(const path savepath) {
  Claims c;
  path teams = savepath / "serverutilities" / "teams";
  if (!exists(teams / "claimedchunks")) return c;
  for (auto &ent : directory_iterator(teams / "claimedchunks")) {
    if (ent.path().extension() != ".dat") continue;
    string id = ent.path().stem().string(), team = id;
    try {
      // the team's title if it has one, else whoever owns it
      if (exists(teams / ent.path().filename())) {
        NbtTag t = loadGzipNbt(teams / ent.path().filename());
        const NbtTag *title = t.get("Title"), *owner = t.get("Owner");
        if (title && !title->s.empty()) team = title->s;
        else if (owner && !owner->s.empty()) team = owner->s;
      }
      NbtTag cc = loadGzipNbt(ent.path());
      const NbtTag *dims = cc.get("ClaimedChunks");
      if (!dims) continue;
      for (auto &[dimname, list] : dims->compound) {
        int dim = atoi(dimname.c_str());
        for (auto &ch : list.list) {
          c.owner[{dim, (int)ch.num("x"), (int)ch.num("z")}] = team;
          c.claimed[{team, dim}]++;
          if (ch.num("loaded")) c.loaded[{team, dim}]++;
        }
      }
    } catch (const exception &e) {
      fprintf(stderr, "claims: failed to read %s: %s\n", ent.path().c_str(), e.what());
    }
  }
  return c;
}

void collect_containers(const path savepath, string &out) {
  // numeric ids are per world, level.dat has the registry ("\x02" items, "\x01" blocks)
  map<int, string> ids;
  try {
    NbtTag level = loadGzipNbt(savepath / "level.dat");
    if (const NbtTag *fml = level.get("FML"))
      if (const NbtTag *data = fml->get("ItemData"))
        for (auto &e : data->list) {
          const NbtTag *k = e.get("K");
          if (!k || k->s.size() < 2) continue;
          int v = (int)e.num("V");
          // an item registration wins over the block of the same id
          if (k->s[0] == '\x02' || !ids.count(v)) ids[v] = k->s.substr(1);
        }
  } catch (const exception &e) {
    fprintf(stderr, "containers: failed to read level.dat: %s\n", e.what());
  }

  Claims claims = load_claims(savepath);
  // (team, dim) -> totals
  map<pair<string, int>, map<ItemKey, int64_t>> items;
  map<pair<string, int>, map<string, int64_t>> tiles;
  map<pair<string, int>, map<pair<string, string>, int64_t>> fluids;
  vector<tuple<string, int, Boiler>> boilers;
  vector<tuple<string, int, GtBoiler>> gt;
  lock_guard<mutex> l(container_lock);
  for_each_dimension(savepath, [&](path directory, int dimension) -> void {
    path regions = directory / "region";
    if (!exists(regions)) return;
    for (auto &ent : directory_iterator(regions)) {
      if (ent.path().extension() != ".mca") continue;
      struct stat st;
      if (stat(ent.path().c_str(), &st) != 0) continue;
      int rx, rz;
      if (sscanf(ent.path().filename().c_str(), "r.%d.%d.mca", &rx, &rz) != 2) continue;
      RegionContainers &r = container_cache[ent.path().string()];
      if (r.mtime != st.st_mtime || r.size != st.st_size) {
        r = RegionContainers{};
        r.mtime = st.st_mtime;
        r.size = st.st_size;
        scan_region(ent.path(), r);
      }
      for (auto &[i, chunk] : r.chunks) {
        auto claim = claims.owner.find({dimension, rx * 32 + (i & 31), rz * 32 + (i >> 5)});
        if (claim == claims.owner.end()) continue;
        pair<string, int> key{claim->second, dimension};
        for (auto &[k, v] : chunk.items) items[key][k] += v;
        for (auto &[k, v] : chunk.tiles) tiles[key][k] += v;
        for (auto &[k, v] : chunk.fluids) fluids[key][k] += v;
        for (auto &b : chunk.boilers) boilers.emplace_back(key.first, dimension, b);
        for (auto &b : chunk.gt_boilers) gt.emplace_back(key.first, dimension, b);
      }
    }
  });

  // display name and registry name of an item
  auto item_label = [&](const ItemKey &k) {
    auto idn = ids.find(k.id);
    string reg = idn != ids.end() ? idn->second : to_string(k.id);
    string name = reg + ":" + to_string(k.damage);
    auto names = item_names.find(reg);
    if (names != item_names.end()) {
      auto n = names->second.find(k.damage);
      if (n != names->second.end()) name = n->second;
    }
    return pair<string, string>{name, reg};
  };

  out += "# HELP container_items Items stored in tile entity inventories (chests, drawers, machines) in claimed chunks, per team and dimension\n# TYPE container_items gauge\n";
  for (auto &[key, m] : items) {
    auto &[team, dim] = key;
    for (auto &[k, count] : m) {
      auto [name, reg] = item_label(k);
      fmt(out, "container_items{team=\"%s\",dim=\"%d\",item=\"%s\",id=\"%s\",damage=\"%d\"} %ld\n",
          esc(team).c_str(), dim, esc(name).c_str(), esc(reg).c_str(), k.damage, count);
    }
  }
  out += "# HELP container_tiles Tile entities holding items in claimed chunks, per team, dimension and type\n# TYPE container_tiles gauge\n";
  for (auto &[key, m] : tiles)
    for (auto &[type, n] : m)
      fmt(out, "container_tiles{team=\"%s\",dim=\"%d\",type=\"%s\"} %ld\n", esc(key.first).c_str(), key.second, esc(type).c_str(), n);
  out += "# HELP container_fluids Fluid stored in tile entities (tanks, machines, pipes) in claimed chunks, in mB, per team, dimension, fluid and tile entity type\n# TYPE container_fluids gauge\n";
  for (auto &[key, m] : fluids)
    for (auto &[k, mb] : m)
      fmt(out, "container_fluids{team=\"%s\",dim=\"%d\",fluid=\"%s\",type=\"%s\"} %ld\n",
          esc(key.first).c_str(), key.second, esc(k.first).c_str(), esc(k.second).c_str(), mb);

  // one series per boiler, there are only ever a handful
  static const struct { const char *name, *help; } boiler_metrics[] = {
    { "railcraft_boiler_steam_mb_per_tick", "Steam a Railcraft boiler makes per tick, from its heat, size and water as of the last save (Railcraft's own formula)" },
    { "railcraft_boiler_heat", "Railcraft boiler heat, it boils from 100" },
    { "railcraft_boiler_max_heat", "Railcraft boiler heat it can reach (500 low pressure, 1000 high pressure)" },
    { "railcraft_boiler_burning", "1 while a Railcraft boiler has fuel burning" },
    { "railcraft_boiler_water_mb", "Water in a Railcraft boiler" },
    { "railcraft_boiler_steam_mb", "Steam waiting in a Railcraft boiler" },
  };
  for (int m = 0; m < (int)size(boiler_metrics); m++) {
    fmt(out, "# HELP %s %s\n# TYPE %s gauge\n", boiler_metrics[m].name, boiler_metrics[m].help, boiler_metrics[m].name);
    for (auto &[team, dim, b] : boilers) {
      double v = m == 0 ? b.steam_per_tick : m == 1 ? b.heat : m == 2 ? b.max_heat : m == 3 ? (b.burning ? 1 : 0) : m == 4 ? b.water : b.steam;
      fmt(out, "%s{team=\"%s\",dim=\"%d\",x=\"%d\",y=\"%d\",z=\"%d\",pressure=\"%s\",tanks=\"%d\"} %g\n", boiler_metrics[m].name,
          esc(team).c_str(), dim, b.x, b.y, b.z, b.pressure.c_str(), b.tanks, v);
    }
  }

  static const struct { const char *name, *help; } gt_metrics[] = {
    { "gt_boiler_steam_l_per_second", "Steam a GregTech boiler makes per second as of the last save, its full rate above 100 degrees with water (NaN for solar, which depends on sunlight)" },
    { "gt_boiler_temperature", "GregTech boiler temperature, it makes steam above 100" },
    { "gt_boiler_max_temperature", "GregTech boiler temperature it can reach" },
    { "gt_boiler_active", "1 while a GregTech boiler is burning fuel" },
    { "gt_boiler_water_l", "Water in a GregTech boiler" },
    { "gt_boiler_steam_l", "Steam waiting in a GregTech boiler" },
  };
  for (int m = 0; m < (int)size(gt_metrics); m++) {
    fmt(out, "# HELP %s %s\n# TYPE %s gauge\n", gt_metrics[m].name, gt_metrics[m].help, gt_metrics[m].name);
    for (auto &[team, dim, b] : gt) {
      double v = m == 0 ? b.steam_per_second : m == 1 ? b.temperature : m == 2 ? b.max_temperature : m == 3 ? (b.active ? 1 : 0) : m == 4 ? b.water : b.steam;
      fmt(out, "%s{team=\"%s\",dim=\"%d\",x=\"%d\",y=\"%d\",z=\"%d\",boiler=\"%s\"} %g\n", gt_metrics[m].name,
          esc(team).c_str(), dim, b.x, b.y, b.z, b.name.c_str(), v);
    }
  }

  out += "# HELP railcraft_boiler_fuel_items Items in a Railcraft boiler's firebox inventory: solid fuel waiting, and a liquid firebox's fluid containers\n# TYPE railcraft_boiler_fuel_items gauge\n";
  for (auto &[team, dim, b] : boilers)
    for (auto &[k, n] : b.fuel_items) {
      auto [name, reg] = item_label(k);
      fmt(out, "railcraft_boiler_fuel_items{team=\"%s\",dim=\"%d\",x=\"%d\",y=\"%d\",z=\"%d\",item=\"%s\",id=\"%s\",damage=\"%d\"} %ld\n",
          esc(team).c_str(), dim, b.x, b.y, b.z, esc(name).c_str(), esc(reg).c_str(), k.damage, n);
    }
  out += "# HELP railcraft_boiler_fuel_mb Liquid fuel in a Railcraft liquid firebox's fuel tank\n# TYPE railcraft_boiler_fuel_mb gauge\n";
  for (auto &[team, dim, b] : boilers)
    for (auto &[fluid, mb] : b.fuel_fluids)
      fmt(out, "railcraft_boiler_fuel_mb{team=\"%s\",dim=\"%d\",x=\"%d\",y=\"%d\",z=\"%d\",fluid=\"%s\"} %ld\n",
          esc(team).c_str(), dim, b.x, b.y, b.z, esc(fluid).c_str(), mb);
  out += "# HELP gt_boiler_fuel_items Items in a GregTech boiler's inventory: fuel waiting, and its ash\n# TYPE gt_boiler_fuel_items gauge\n";
  for (auto &[team, dim, b] : gt)
    for (auto &[k, n] : b.fuel_items) {
      auto [name, reg] = item_label(k);
      fmt(out, "gt_boiler_fuel_items{team=\"%s\",dim=\"%d\",x=\"%d\",y=\"%d\",z=\"%d\",boiler=\"%s\",item=\"%s\",id=\"%s\",damage=\"%d\"} %ld\n",
          esc(team).c_str(), dim, b.x, b.y, b.z, b.name.c_str(), esc(name).c_str(), esc(reg).c_str(), k.damage, n);
    }

  out += "# HELP claimed_chunks Chunks claimed with ServerUtilities, per team and dimension\n# TYPE claimed_chunks gauge\n";
  for (auto &[key, n] : claims.claimed)
    fmt(out, "claimed_chunks{team=\"%s\",dim=\"%d\"} %ld\n", esc(key.first).c_str(), key.second, n);
  out += "# HELP claimed_chunks_loaded Claimed chunks that are also force loaded, per team and dimension\n# TYPE claimed_chunks_loaded gauge\n";
  for (auto &[key, n] : claims.claimed)
    fmt(out, "claimed_chunks_loaded{team=\"%s\",dim=\"%d\"} %ld\n", esc(key.first).c_str(), key.second,
        claims.loaded.count(key) ? claims.loaded.at(key) : 0);
}

void collect_all(const path savepath, string &out) {
  auto start = chrono::steady_clock::now();
  collect_pollution(savepath, out);
  collect_world(savepath, out);
  collect_players(savepath, out);
  collect_containers(savepath, out);
  fmt(out, "# HELP exporter_scrape_duration_seconds Time spent reading the save\n# TYPE exporter_scrape_duration_seconds gauge\nexporter_scrape_duration_seconds %f\n",
      chrono::duration<double>(chrono::steady_clock::now() - start).count());
}

static void usage(const char *argv0) {
  printf("usage: %s -p <save path> [-l [addr:]port] [-o <textfile.prom>] [-n <items.json>]\n"
         "  -n  texture-dump's items.json, gives container_items real names (\"Iron Ingot\")\n"
         "  -l  serve prometheus metrics over http (default :9150)\n"
         "  -o  write a node_exporter textfile every 10s. with -o alone there is no http server,\n"
         "      with both -o and -l it does both\n", argv0);
}

int main(int argc, char **argv) {
  char *savepath = NULL;
  char *prom_out = NULL;
  string listen_addr;
  int port = 9150;
  bool listen = false;
  int opt;
  while ((opt = getopt(argc, argv, "p:o:l:n:h")) != -1) {
    switch (opt) {
    case 'n':
      try {
        // keep the document alive, items() only refers into it
        json names = json::parse(readFile(optarg));
        for (auto &[reg, damages] : names.items())
          for (auto &[damage, name] : damages.items()) item_names[reg][atoi(damage.c_str())] = name.get<string>();
        printf("loaded names for %zu items from %s\n", item_names.size(), optarg);
      } catch (const exception &e) {
        fprintf(stderr, "failed to load %s: %s\n", optarg, e.what());
      }
      break;
    case 'p':
      savepath = optarg;
      break;
    case 'o':
      prom_out = optarg;
      break;
    case 'l': {
      listen = true;
      string l = optarg;
      size_t colon = l.rfind(':');
      if (colon == string::npos) {
        port = atoi(l.c_str());
      } else {
        listen_addr = l.substr(0, colon);
        if (listen_addr.size() >= 2 && listen_addr.front() == '[' && listen_addr.back() == ']')
          listen_addr = listen_addr.substr(1, listen_addr.size() - 2);
        port = atoi(l.c_str() + colon + 1);
      }
      break;
    }
    default:
      usage(argv[0]);
      return 1;
    }
  }
  if (!savepath) {
    puts("error, specify a save path with `-p <path>`");
    usage(argv[0]);
    return 1;
  }
  const path save{savepath};
  // the textfile, like the original exporter wrote for node_exporter's textfile collector
  auto write_textfile = [save, prom_out]() {
    const string tmp = string(prom_out) + ".tmp";
    while (true) {
      string out;
      collect_all(save, out);
      FILE *fh = fopen(tmp.c_str(), "w");
      if (fh) {
        fwrite(out.data(), 1, out.size(), fh);
        fclose(fh);
        // atomic replace so node_exporter never reads a half written file
        rename(tmp.c_str(), prom_out);
      } else {
        perror(tmp.c_str());
      }
      sleep(10);
    }
  };
  if (prom_out && !listen) {
    write_textfile();
    return 0;
  }
  // collecting only reads the save, the file and http scrapes can run side by side
  if (prom_out) thread(write_textfile).detach();
  MetricsServer server(listen_addr, port);
  server.addCollector([save](string &out) { collect_all(save, out); });
  server.serve();
  return 0;
}

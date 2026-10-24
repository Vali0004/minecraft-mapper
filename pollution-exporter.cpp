// SPDX-License-Identifier: MPL-2.0
#include <arpa/inet.h>
#include <assert.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "utils.h"
#include "metrics_http.h"
#include "nbt_tree.h"

#include <chrono>
#include <cstdarg>
#include <map>
#include <nlohmann/json.hpp>

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

void collect_all(const path savepath, string &out) {
  auto start = chrono::steady_clock::now();
  collect_pollution(savepath, out);
  collect_world(savepath, out);
  collect_players(savepath, out);
  fmt(out, "# HELP exporter_scrape_duration_seconds Time spent reading the save\n# TYPE exporter_scrape_duration_seconds gauge\nexporter_scrape_duration_seconds %f\n",
      chrono::duration<double>(chrono::steady_clock::now() - start).count());
}

static void usage(const char *argv0) {
  printf("usage: %s -p <save path> [-l [addr:]port] [-o <textfile.prom>]\n"
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
  while ((opt = getopt(argc, argv, "p:o:l:h")) != -1) {
    switch (opt) {
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

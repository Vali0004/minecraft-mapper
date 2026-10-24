// SPDX-License-Identifier: MPL-2.0
// keeps a history of biome-thingy's rendered tiles in sqlite, see tile_db.cpp
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "metrics_http.h"
#include "tile_db.h"
#include "utils.h"

using namespace std;
using namespace filesystem;

static void list(Db &db) {
  sqlite3_stmt *s = db.prep(
    "SELECT s.id, datetime(s.taken_at, 'unixepoch', 'localtime'), "
    "  (SELECT count(*) FROM tile_versions v WHERE v.snapshot_id = s.id), "
    "  (SELECT coalesce(sum(length(png)), 0) FROM tile_versions v WHERE v.snapshot_id = s.id), "
    "  coalesce(s.note, '') "
    "FROM snapshots s ORDER BY s.id");
  printf("%6s  %-19s  %8s  %10s  %s\n", "id", "taken", "changed", "bytes", "note");
  while (sqlite3_step(s) == SQLITE_ROW) {
    printf("%6lld  %-19s  %8lld  %10lld  %s\n", sqlite3_column_int64(s, 0), sqlite3_column_text(s, 1),
           sqlite3_column_int64(s, 2), sqlite3_column_int64(s, 3), sqlite3_column_text(s, 4));
  }
  sqlite3_finalize(s);
}

// writes the map as it looked at snapshot `sid` into `out`
static void export_snapshot(Db &db, int64_t sid, const path &out) {
  sqlite3_stmt *s = db.prep(
    "SELECT v.path, v.png FROM tile_versions v "
    "JOIN (SELECT path, max(snapshot_id) AS sid FROM tile_versions WHERE snapshot_id <= ?1 GROUP BY path) m "
    "  ON v.path = m.path AND v.snapshot_id = m.sid "
    "WHERE v.png IS NOT NULL");
  sqlite3_bind_int64(s, 1, sid);
  int n = 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    path rel = (const char*)sqlite3_column_text(s, 0);
    if (rel.is_absolute() || rel.string().find("..") != string::npos) continue;
    path dst = out / rel;
    create_directories(dst.parent_path());
    to_file(dst, sqlite3_column_blob(s, 1), sqlite3_column_bytes(s, 1));
    n++;
  }
  sqlite3_finalize(s);
  printf("exported %d tiles of snapshot %ld to %s\n", n, sid, out.c_str());
}


static string json_escape(const string &in) {
  string out;
  for (char c : in) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += c;
  }
  return out;
}

static const char *content_type(const path &p) {
  string e = p.extension().string();
  if (e == ".html") return "text/html; charset=utf-8";
  if (e == ".js") return "text/javascript";
  if (e == ".wasm") return "application/wasm";
  if (e == ".css") return "text/css";
  if (e == ".png") return "image/png";
  return "application/octet-stream";
}

// serves the tile history to the web viewer:
//   /api/snapshots        [{"id", "taken_at", "note"}], oldest first
//   /api/dims             ["", "DIM-1", ...], "" being the overworld (its heights live under height/)
//   /tile/<sid>/<path>    the newest version of <path> at or before snapshot <sid>
//   /textures/<file>      atlas.png and blocks.json from a texture-dump directory, for the textured view
//   anything else         a file from `webroot`
static void serve(Db &db, int port, const path &webroot, const path &textures) {
  MetricsServer http("", port);
  sqlite3_stmt *snaps = db.prep("SELECT id, taken_at, coalesce(note, '') FROM snapshots ORDER BY id");
  sqlite3_stmt *dims = db.prep(
    "SELECT DISTINCT CASE WHEN path GLOB '[0-9]*' OR path GLOB 'height/*' OR path GLOB 'icons/*' OR path GLOB 'shapes/*' OR path GLOB 'caves/*' THEN '' ELSE substr(path, 1, instr(path, '/') - 1) END "
    "FROM tile_versions ORDER BY 1");
  sqlite3_stmt *tile = db.prep(
    "SELECT png FROM tile_versions WHERE path = ? AND snapshot_id <= ? ORDER BY snapshot_id DESC LIMIT 1");

  http.addHandler([&](const string &target, MetricsServer::Response &r) {
    if (target == "/api/snapshots") {
      r.body = "[";
      while (sqlite3_step(snaps) == SQLITE_ROW) {
        if (r.body.size() > 1) r.body += ",";
        r.body += "{\"id\":" + to_string(sqlite3_column_int64(snaps, 0)) +
          ",\"taken_at\":" + to_string(sqlite3_column_int64(snaps, 1)) +
          ",\"note\":\"" + json_escape((const char*)sqlite3_column_text(snaps, 2)) + "\"}";
      }
      sqlite3_reset(snaps);
      r.body += "]";
      r.status = "200 OK"; r.ctype = "application/json";
      return true;
    }
    if (target == "/api/dims") {
      r.body = "[";
      while (sqlite3_step(dims) == SQLITE_ROW) {
        if (r.body.size() > 1) r.body += ",";
        r.body += "\"" + json_escape((const char*)sqlite3_column_text(dims, 0)) + "\"";
      }
      sqlite3_reset(dims);
      r.body += "]";
      r.status = "200 OK"; r.ctype = "application/json";
      return true;
    }
    if (target.starts_with("/tile/")) {
      size_t slash = target.find('/', 6);
      if (slash == string::npos) return false;
      int64_t sid = atoll(target.substr(6, slash - 6).c_str());
      string rel = target.substr(slash + 1);
      sqlite3_bind_text(tile, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(tile, 2, sid);
      if (sqlite3_step(tile) == SQLITE_ROW && sqlite3_column_type(tile, 0) == SQLITE_BLOB) {
        r.body.assign((const char*)sqlite3_column_blob(tile, 0), sqlite3_column_bytes(tile, 0));
        r.status = "200 OK"; r.ctype = "image/png";
      } else {
        r.status = "404 Not Found"; r.ctype = "text/plain"; r.body = "no such tile\n";
      }
      sqlite3_reset(tile);
      // a tile at an existing snapshot never changes, missing ones may appear in a later ingest
      r.headers = r.status[0] == '2' ? "Cache-Control: public, max-age=86400\r\n" : "Cache-Control: max-age=60\r\n";
      return true;
    }
    if (target.find("..") != string::npos) return false;
    path file;
    if (target == "/textures/atlas.png" || target == "/textures/blocks.json") {
      if (textures.empty()) return false;
      file = textures / target.substr(strlen("/textures/"));
    } else {
      if (webroot.empty()) return false;
      file = webroot / (target == "/" ? "index.html" : target.substr(1));
    }
    if (!is_regular_file(file)) return false;
    r.body = readFile(file);
    r.status = "200 OK"; r.ctype = content_type(file);
    return true;
  });
  http.serve();
}

static void usage(const char *argv0) {
  printf("usage:\n"
         "  %1$s -d <db> ingest -o <render dir> [-n note] [-t unix time]   snapshot changed tiles\n"
         "  %1$s -d <db> list                               list snapshots\n"
         "  %1$s -d <db> export -s <id> -o <dir>            write the map as of a snapshot\n"
         "  %1$s -d <db> serve -l <port> [-w <web dir>] [-x <texture-dump dir>]   serve tiles and the viewer over http\n", argv0);
}

int main(int argc, char **argv) {
  const char *dbpath = nullptr, *dir = nullptr, *note = nullptr;
  int64_t sid = -1, taken_at = time(nullptr);
  int port = 0;
  const char *webroot = "", *textures = "";
  int opt;
  // allow options before and after the command
  while ((opt = getopt(argc, argv, "+d:o:n:s:t:l:w:x:h")) != -1 || optind < argc) {
    if (opt == -1) break;
    switch (opt) {
    case 'd': dbpath = optarg; break;
    case 'o': dir = optarg; break;
    case 'n': note = optarg; break;
    case 's': sid = atoll(optarg); break;
    case 't': taken_at = atoll(optarg); break;
    case 'l': port = atoi(optarg); break;
    case 'w': webroot = optarg; break;
    case 'x': textures = optarg; break;
    default: usage(argv[0]); return 1;
    }
  }
  if (!dbpath || optind >= argc) { usage(argv[0]); return 1; }
  string cmd = argv[optind];
  optind++;
  while ((opt = getopt(argc, argv, "d:o:n:s:t:l:w:x:")) != -1) {
    switch (opt) {
    case 'o': dir = optarg; break;
    case 'n': note = optarg; break;
    case 's': sid = atoll(optarg); break;
    case 't': taken_at = atoll(optarg); break;
    case 'l': port = atoi(optarg); break;
    case 'w': webroot = optarg; break;
    case 'x': textures = optarg; break;
    default: usage(argv[0]); return 1;
    }
  }

  Db db(dbpath);
  if (cmd == "ingest" && dir) ingest(db, dir, note, taken_at);
  else if (cmd == "list") list(db);
  else if (cmd == "export" && dir && sid >= 0) export_snapshot(db, sid, dir);
  else if (cmd == "serve" && port > 0) serve(db, port, webroot, textures);
  else { usage(argv[0]); return 1; }
  return 0;
}

// SPDX-License-Identifier: MPL-2.0
// see tile_db.h
//
// every `ingest` creates a snapshot, but only stores tiles whose contents changed since
// the previous version of that tile. the map at snapshot N is: for every path, the newest
// version with snapshot_id <= N (a NULL png means the tile was deleted)
#include <chrono>
#include <stdio.h>
#include <string.h>

#include "tile_db.h"
#include "utils.h"

using namespace std;
using namespace filesystem;

static const char *schema = R"(
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS snapshots (
  id INTEGER PRIMARY KEY,
  taken_at INTEGER NOT NULL, -- unix seconds
  note TEXT
);
CREATE TABLE IF NOT EXISTS tile_versions (
  path TEXT NOT NULL,         -- relative to the render dir, e.g. DIM-1/r.0.0.png
  snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),
  png BLOB,                   -- NULL = tile removed
  PRIMARY KEY (path, snapshot_id)
) WITHOUT ROWID;
-- last seen mtime/size per tile, so ingest can skip files without reading them
CREATE TABLE IF NOT EXISTS tile_state (
  path TEXT PRIMARY KEY,
  mtime INTEGER NOT NULL,
  size INTEGER NOT NULL
) WITHOUT ROWID;
)";

Db::Db(const string &p) {
  if (sqlite3_open(p.c_str(), &db) != SQLITE_OK) die("open");
  sqlite3_busy_timeout(db, 10000);
  exec(schema);
}

void Db::die(const char *what) {
  fprintf(stderr, "sqlite %s: %s\n", what, sqlite3_errmsg(db));
  exit(1);
}

void Db::exec(const char *sql) {
  char *err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    fprintf(stderr, "sqlite: %s\n", err);
    exit(1);
  }
}

sqlite3_stmt *Db::prep(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) die("prepare");
  return s;
}

bool has_snapshot_note(Db &db, const string &note) {
  sqlite3_stmt *s = db.prep("SELECT 1 FROM snapshots WHERE note = ?");
  sqlite3_bind_text(s, 1, note.c_str(), -1, SQLITE_TRANSIENT);
  bool found = sqlite3_step(s) == SQLITE_ROW;
  sqlite3_finalize(s);
  return found;
}

static int64_t mtime_ns(const path &p) {
  return chrono::duration_cast<chrono::nanoseconds>(last_write_time(p).time_since_epoch()).count();
}

int64_t ingest(Db &db, const path &dir, const char *note, int64_t taken_at) {
  db.exec("BEGIN IMMEDIATE");
  sqlite3_stmt *snap = db.prep("INSERT INTO snapshots (taken_at, note) VALUES (?, ?)");
  sqlite3_bind_int64(snap, 1, taken_at);
  if (note) sqlite3_bind_text(snap, 2, note, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(snap) != SQLITE_DONE) db.die("insert snapshot");
  sqlite3_finalize(snap);
  int64_t sid = sqlite3_last_insert_rowid(db.db);

  sqlite3_stmt *state = db.prep("SELECT mtime, size FROM tile_state WHERE path = ?");
  sqlite3_stmt *latest = db.prep("SELECT png FROM tile_versions WHERE path = ? ORDER BY snapshot_id DESC LIMIT 1");
  sqlite3_stmt *ins = db.prep("INSERT INTO tile_versions (path, snapshot_id, png) VALUES (?, ?, ?)");
  sqlite3_stmt *upd = db.prep("INSERT OR REPLACE INTO tile_state (path, mtime, size) VALUES (?, ?, ?)");
  db.exec("CREATE TEMP TABLE seen (path TEXT PRIMARY KEY) WITHOUT ROWID");
  sqlite3_stmt *seen = db.prep("INSERT INTO seen VALUES (?)");

  int scanned = 0, changed = 0, touched = 0;
  for (auto &ent : recursive_directory_iterator(dir)) {
    if (!ent.is_regular_file() || ent.path().extension() != ".png") continue;
    const string rel = relative(ent.path(), dir).string();
    const int64_t mt = mtime_ns(ent.path());
    const int64_t sz = ent.file_size();
    scanned++;

    sqlite3_bind_text(seen, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(seen);
    sqlite3_reset(seen);

    sqlite3_bind_text(state, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    bool same_stat = sqlite3_step(state) == SQLITE_ROW &&
      sqlite3_column_int64(state, 0) == mt && sqlite3_column_int64(state, 1) == sz;
    sqlite3_reset(state);
    if (same_stat) continue;

    // rewritten on disk, but possibly with identical pixels (region saved without changes)
    const string data = readFile(ent.path());
    sqlite3_bind_text(latest, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    bool same_data = false;
    if (sqlite3_step(latest) == SQLITE_ROW && sqlite3_column_type(latest, 0) == SQLITE_BLOB) {
      same_data = sqlite3_column_bytes(latest, 0) == (int)data.size() &&
        memcmp(sqlite3_column_blob(latest, 0), data.data(), data.size()) == 0;
    }
    sqlite3_reset(latest);

    if (same_data) {
      touched++;
    } else {
      sqlite3_bind_text(ins, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(ins, 2, sid);
      sqlite3_bind_blob(ins, 3, data.data(), data.size(), SQLITE_TRANSIENT);
      if (sqlite3_step(ins) != SQLITE_DONE) db.die("insert tile");
      sqlite3_reset(ins);
      changed++;
    }
    sqlite3_bind_text(upd, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, mt);
    sqlite3_bind_int64(upd, 3, sz);
    if (sqlite3_step(upd) != SQLITE_DONE) db.die("update state");
    sqlite3_reset(upd);
  }

  // tiles that disappeared from disk get a tombstone
  sqlite3_stmt *gone = db.prep("SELECT path FROM tile_state WHERE path NOT IN (SELECT path FROM temp.seen)");
  int removed = 0;
  while (sqlite3_step(gone) == SQLITE_ROW) {
    sqlite3_bind_text(ins, 1, (const char*)sqlite3_column_text(gone, 0), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, sid);
    sqlite3_bind_null(ins, 3);
    if (sqlite3_step(ins) != SQLITE_DONE) db.die("insert tombstone");
    sqlite3_reset(ins);
    removed++;
  }
  sqlite3_finalize(gone);
  db.exec("DELETE FROM tile_state WHERE path NOT IN (SELECT path FROM temp.seen)");

  for (auto s : {state, latest, ins, upd, seen}) sqlite3_finalize(s);
  db.exec("DROP TABLE temp.seen");
  db.exec("COMMIT");
  printf("snapshot %ld: %d tiles scanned, %d changed, %d rewritten but identical, %d removed\n",
         sid, scanned, changed, touched, removed);
  return sid;
}

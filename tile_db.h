// SPDX-License-Identifier: MPL-2.0
#pragma once
// sqlite store of rendered map tiles, shared by tile-history and biome-thingy's backup mode
#include <filesystem>
#include <sqlite3.h>
#include <stdint.h>
#include <string>

struct Db {
  sqlite3 *db = nullptr;
  Db(const std::string &p);
  ~Db() { sqlite3_close(db); }
  [[noreturn]] void die(const char *what);
  void exec(const char *sql);
  sqlite3_stmt *prep(const char *sql);
};

// snapshot every changed tile under `dir`, returns the new snapshot id
int64_t ingest(Db &db, const std::filesystem::path &dir, const char *note, int64_t taken_at);
// true if a snapshot with this note exists, used to skip already indexed backups
bool has_snapshot_note(Db &db, const std::string &note);

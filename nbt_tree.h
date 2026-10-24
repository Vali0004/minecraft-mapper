// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// small DOM style NBT reader, for when the callback parser in nbt.h is not enough
struct NbtTag {
  uint8_t type = 0;
  int64_t i = 0;     // tags 1-4
  double d = 0;      // tags 5-6
  std::string s;     // tag 8
  std::vector<NbtTag> list;  // tag 9, also int/long/byte arrays (as numbers)
  std::map<std::string, NbtTag> compound;  // tag 10

  const NbtTag *get(const std::string &key) const;
  // numeric value of any number tag, or def if missing
  double num(const std::string &key, double def = 0) const;
  double asNum() const { return (type == 5 || type == 6) ? d : (double)i; }
};

// parses an uncompressed NBT blob, throws std::runtime_error on bad data
NbtTag parseNbtTree(const std::string &data);
// reads a gzip compressed NBT file (level.dat, playerdata/*.dat)
NbtTag loadGzipNbt(const std::filesystem::path &p);

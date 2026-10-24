// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <string>
#include <utility>

typedef struct {
  uint32_t chunk_offsets[32][32];
  uint32_t chunk_lastmods[32][32];
} region_header;

std::pair<int, int> parse_region_name(std::string name);

class Region {
public:
  Region(std::filesystem::path dimDir, int dimension, int x, int z);
  ~Region();

  void reload_header();

  region_header *header;
  int dim;
private:
  int x, z;
  FILE *fd;
};

// SPDX-License-Identifier: MPL-2.0
#include "region.h"

#include <assert.h>
#include <unistd.h>

using namespace std;

pair<int, int> parse_region_name(string name) {
  string no_r_name = name.substr(2);
  size_t split_pos = no_r_name.find(".");
  if (split_pos != string::npos) {
    string x_coord = no_r_name.substr(0, split_pos);
    int x = atoi(x_coord.c_str());
    string z_coord = no_r_name.substr(split_pos + 1);
    int z = atoi(z_coord.c_str());

    //printf("%s == %d %d, x %d<->%d, z %d<->%d\n", name.c_str(), x, z, x << 9, (x+1) << 9, z << 9, (z+1) << 9);

    return {x, z};
  }
  return {0,0};
}

Region::Region(filesystem::path dimDir, int dimension, int x, int z) : x(x), z(z), header(NULL), dim(dimension) {
  char buffer[32];
  auto region_path = dimDir / "region";

  snprintf(buffer, 30, "r.%d.%d.mca", x, z);
  region_path = region_path / buffer;

  printf("%s\n", region_path.c_str());
  fd = fopen(region_path.c_str(), "r");
  assert(fd);
}

void Region::reload_header() {
  if (!header) header = (region_header*)malloc(sizeof(*header));
  int hnd = fileno(fd);
  int size = pread(hnd, header, sizeof(*header), 0);
  assert(size == sizeof(*header));
}

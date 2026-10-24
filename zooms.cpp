// SPDX-License-Identifier: MPL-2.0
#include "zooms.h"
#include "utils.h"

#include <assert.h>
#include <iostream>
#include <set>
#include <stdint.h>
#include <string.h>

using namespace std;
namespace fs = std::filesystem;

uint32_t get_pixel_color(uint8_t *buffer, int x, int y, int color) {
  int row_off = y * 512 * 3;
  int off = row_off + (x * 3);
  return buffer[off + color];
}

uint32_t shrink_pixel(const Image &input, int x, int y) {
  uint32_t a = input.getPixel(x+0, y+0);
  uint32_t b = input.getPixel(x+0, y+1);
  uint32_t c = input.getPixel(x+1, y+0);
  uint32_t d = input.getPixel(x+1, y+1);

  uint32_t out = 0x0; // alpha
  for (int i=0; i<4; i++) {
    uint32_t accumulator = 0;
    accumulator += (a >> (i*8)) & 0xff;
    accumulator += (b >> (i*8)) & 0xff;
    accumulator += (c >> (i*8)) & 0xff;
    accumulator += (d >> (i*8)) & 0xff;
    accumulator = accumulator>>2;

    out |= (accumulator << (i*8));
  }

  return out;
}

void set_pixel(uint8_t *buffer, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  int row_off = y * 512 * 3;
  int off = row_off + (x * 3);
  buffer[off+0] = r;
  buffer[off+1] = g;
  buffer[off+2] = b;
}

// create a 256x256 image
static inline void clear_quadrant(uint32_t *out, int qx, int qy) {
  uint32_t *base = out + (qy * 256) * 512 + (qx * 256);
  for (int r = 0; r < 256; r++) {
    ::memset(base + r * 512, 0, 256 * sizeof(uint32_t));
  }
}

// Quick hack for doing (a+b)/2 with per-lane rounding
static inline uint32_t avg2_u8x4(uint32_t x, uint32_t y) {
  return ((x & 0xFEFEFEFEu) >> 1)
       + ((y & 0xFEFEFEFEu) >> 1)
       +  (x & y & 0x01010101u);
}

// Average a 2x2 block, (a,b,c,d)/4
static inline uint32_t avg2x2_u8x4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return avg2_u8x4(avg2_u8x4(a, c), avg2_u8x4(b, d));
}

static inline void downsample_into_quadrant(const uint32_t *input, uint32_t *out, int qx, int qy) {
  uint32_t *out_base = out + (qy * 256) * 512 + (qx * 256);

  for (int y=0; y<512; y += 2) {
    const uint32_t *row0 = input + (y + 0) * 512;
    const uint32_t *row1 = input + (y + 1) * 512;
    uint32_t *out_row = out_base + (y >> 1) * 512;

    for (int x=0; x<512; x += 2) {
      uint32_t a = row0[x + 0];
      uint32_t c = row0[x + 1];
      uint32_t b = row1[x + 0];
      uint32_t d = row1[x + 1];
      out_row[x >> 1] = avg2x2_u8x4(a, b, c, d);
    }
  }
}

int shrink_tile(filesystem::path path, int xin, int yin, int tilex, int tiley, Image &input_buffer, Image &output_buffer, int zoom) {
  char buffer[64];
  snprintf(buffer, 60, "%d/%d,%d.png", zoom, xin + tilex, yin + tiley);
  string input = path / buffer;

  int ret = load_png(input, input_buffer);

  //printf("shrinking %s into tile %d,%d, found %d\n", input.c_str(), tilex, tiley, ret);

  if (ret == 0) {
    clear_quadrant(output_buffer.bitmap, tilex, tiley);
    return ret;
  }

  //for (int x=0; x<512; x += 2) {
  //  for (int y=0; y<512; y += 2) {
  //    uint32_t pix = shrink_pixel(input_buffer, x, y);
  //    output_buffer.setPixel((tilex*256) + (x>>1), (tiley*256) + (y>>1), pix);
  //  }
  //}
  downsample_into_quadrant(input_buffer.bitmap, output_buffer.bitmap, tilex, tiley);

  return ret;
}

pair<int, int> parse_tile_name(string name) {
  size_t split_pos = name.find(",");
  if (split_pos != string::npos) {
    string x_coord = name.substr(0, split_pos);
    string z_coord = name.substr(split_pos+1);

    int x = atoi(x_coord.c_str());
    int z = atoi(z_coord.c_str());
    return {x,z};
  }
  assert(0);
  return {0,0};
}

void regen_zooms(const filesystem::path &path, bool verbose) {
  const int maxzoom = 17;
  char buffer[64];

  for (int zoom=16; zoom > 5; zoom--) {
    snprintf(buffer, 60, "%d", zoom+1);
    filesystem::directory_iterator iterator{ path / buffer };

    fs::path parentDir = path / to_string(zoom);
    fs::create_directories(parentDir);

    set<pair<int, int>> todo_set;

    const int steps = maxzoom - zoom;
    if (!quiet) printf("generating %d from %d, steps %d\n", zoom, zoom+1, steps);
    for (auto &ent : iterator) {
      auto &p = ent.path();
      auto coord = parse_tile_name(p.stem());
      pair<int,int> parent_coord = { coord.first >> 1, coord.second >> 1 };
      todo_set.insert(parent_coord);
    }

    // every parent tile only reads its own 4 children, so a level builds in parallel
    vector<pair<int, int>> todo_list(todo_set.begin(), todo_set.end());
    parallel_for(todo_list.size(), [&](size_t i) {
      thread_local Image input_buffer;
      thread_local Image output_buffer;
      int x = todo_list[i].first;
      int y = todo_list[i].second;
      int xin = x << 1;
      int yin = y << 1;

      int parts = 0;

      parts += shrink_tile(path, xin, yin, 0, 0, input_buffer, output_buffer, zoom+1);
      parts += shrink_tile(path, xin, yin, 0, 1, input_buffer, output_buffer, zoom+1);
      parts += shrink_tile(path, xin, yin, 1, 0, input_buffer, output_buffer, zoom+1);
      parts += shrink_tile(path, xin, yin, 1, 1, input_buffer, output_buffer, zoom+1);

      if (parts > 0) {
        char name[64];
        snprintf(name, 60, "%d/%d,%d.png", zoom, x, y);
        write_png(path / name, output_buffer);
      }
    });
  }
}

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <stdint.h>

class Image {
public:
  Image();
  ~Image();
  void setPixel(int x, int y, uint32_t color) {
    bitmap[(y * 512) + x] = color;
  }
  uint32_t getPixel(int x, int y) const {
    return bitmap[(y * 512) + x];
  }

  uint32_t *bitmap;
};

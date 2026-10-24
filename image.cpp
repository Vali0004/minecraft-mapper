// SPDX-License-Identifier: MPL-2.0
#include "image.h"

Image::Image() {
  bitmap = new uint32_t[512*512];
}

Image::~Image() {
  delete[] bitmap;
}

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <stdint.h>
#include <string.h>

class Section {
public:
  Section() {
    memset(blocks, 0, sizeof(blocks));
    memset(meta, 0, sizeof(meta));
  }

  uint16_t blocks[16][16][16]; // x y z
  uint16_t meta[16][16][16];   // x y z
  bool has_data16 = false;     // NotEnoughIDs writes both, its 16 bit metadata wins
};

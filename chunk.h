// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "section.h"

#include <map>
#include <set>
#include <vector>
#include <string>

class Chunk {
public:
  // X neg=west, pos=east
  // Y neg=down, pos=up
  // Z neg=north, pos=south
  Chunk();
  ~Chunk();
  void reset();
  void updateAddBlocks(int section, const uint8_t data[16][16][8]);
  void updateBlocks(int section, const uint8_t data[16][16][16]);
  void updateBlocks16(int section, const uint16_t data[16][16][16]);
  // vanilla nibble metadata, and NotEnoughIDs' 16 bit Data16 (which wins when both are present)
  void updateData(int section, const uint8_t data[16][16][8]);
  void updateData16(int section, const uint16_t data[16][16][16]);
  void printSection(int section_num);
  // height gets the y of each top block and topmeta its metadata, 0 where the column is empty
  // the scan starts at startY. with skipSolid it first goes down through solid blocks there (the nether's
  // roof, the rock above a cave) and reports the floor of the first open space below instead
  void getTopMostBlocks(uint16_t toplayer[16][16], uint16_t topmeta[16][16], uint8_t height[16][16], const std::set<std::string> &ignoredBlocks, const std::map<int32_t,std::string> &blockMap, int startY = 255, bool skipSolid = false);
  // y of the highest block that roofs a column: not see-through, and not part of a tree. a cave
  // floor has to be below it, the ground under a forest canopy is not a cave
  // notCover[id] is 1 for blocks that don't count, see level_state::not_cover
  int coverHeight(int x, int z, const std::vector<uint8_t> &notCover) const;
private:
  void ensureSection(int section);
  uint16_t blockAt(int x, int y, int z) const;
  Section *sections[16];
};

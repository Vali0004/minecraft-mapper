// SPDX-License-Identifier: MPL-2.0
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

#include "chunk.h"

using namespace std;

Chunk::Chunk() {
  for (int i=0; i<16; i++) {
    sections[i] = NULL;
  }
}

Chunk::~Chunk() {
  reset();
}

void Chunk::ensureSection(int section) {
  if (sections[section] == NULL) {
    sections[section] = new Section();
  }
}

void Chunk::updateBlocks(int section_num, const uint8_t data[16][16][16]) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  for (int x=0; x<16; x++) {
    for (int y=0; y<16; y++) {
      for (int z=0; z<16; z++) {
        uint16_t old = section->blocks[x][y][z];
        old &= 0xff00;
        old |= data[y][z][x];
        section->blocks[x][y][z] = old;
        //if ((x==6) && (z==9)) printf("%d %d %d, %d/%x\n", x, (section_num*16)+y, z, data[y][x][z], data[y][x][z]);
      }
    }
  }
}

void Chunk::updateBlocks16(int section_num, const uint16_t data[16][16][16]) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  for (int x=0; x<16; x++) {
    for (int y=0; y<16; y++) {
      for (int z=0; z<16; z++) {
        section->blocks[x][y][z] = ntohs(data[y][z][x]);
        //if ((x==6) && (z==9)) printf("%d %d %d, %d/%x\n", x, (section_num*16)+y, z, data[y][x][z], data[y][x][z]);
      }
    }
  }
}

void Chunk::updateAddBlocks(int section_num, const uint8_t data[16][16][8]) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  for (int x=0; x<16; x++) {
    for (int y=0; y<16; y++) {
      for (int z=0; z<16; z++) {
        uint16_t old = section->blocks[x][y][z];
        old &= 0xff;
        uint8_t byte = data[y][z][x/2];
        if ((x%2) == 0) byte &= 0xf;
        else byte = byte >> 4;
        old |= byte << 8;
        section->blocks[x][y][z] = old;
      }
    }
  }
}

void Chunk::updateData(int section_num, const uint8_t data[16][16][8]) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  if (section->has_data16) return;
  for (int x=0; x<16; x++) {
    for (int y=0; y<16; y++) {
      for (int z=0; z<16; z++) {
        uint8_t byte = data[y][z][x/2];
        section->meta[x][y][z] = (x % 2) == 0 ? (byte & 0xf) : (byte >> 4);
      }
    }
  }
}

void Chunk::updateData16(int section_num, const uint16_t data[16][16][16]) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  section->has_data16 = true;
  for (int x=0; x<16; x++) {
    for (int y=0; y<16; y++) {
      for (int z=0; z<16; z++) {
        section->meta[x][y][z] = ntohs(data[y][z][x]);
      }
    }
  }
}

void Chunk::printSection(int section_num) {
  ensureSection(section_num);
  Section *section = sections[section_num];
  for (int y=0; y<16; y++) {
    printf("section-y %d, y %d\n", y, (section_num*16)+y);
    for (int z=0; z<16; z++) {
      printf("z %2d  ", z);
      for (int x=0; x<16; x++) {
        printf("%3d ", section->blocks[x][y][z]);
      }
      puts("");
    }
  }
}

uint16_t Chunk::blockAt(int x, int y, int z) const {
  const Section *section = sections[y >> 4];
  return section ? section->blocks[x][y & 15][z] : 0;
}

void Chunk::getTopMostBlocks(uint16_t toplayer[16][16], uint16_t topmeta[16][16], uint8_t height[16][16], const set<string> &ignoredBlocks, const std::map<int32_t,std::string> &blockMap, int startY, bool skipSolid) {
  auto ignored = [&](uint16_t block) {
    if (block == 0) return true; // air
    auto name = blockMap.find(block);
    return name != blockMap.end() && ignoredBlocks.count(name->second);
  };

  for (int x=0; x<16; x++) {
    for (int z=0; z<16; z++) {
      toplayer[x][z] = 0;
      topmeta[x][z] = 0;
      height[x][z] = 0;
      int y = startY;
      if (skipSolid) {
        // down through the roof, then through the open space below it
        while (y >= 0 && ignored(blockAt(x, y, z))) y--;
        while (y >= 0 && !ignored(blockAt(x, y, z))) y--;
      }
      for (; y >= 0; y--) {
        uint16_t block = blockAt(x, y, z);
        if (ignored(block)) continue;
        toplayer[x][z] = block;
        topmeta[x][z] = sections[y >> 4]->meta[x][y & 15][z];
        height[x][z] = y;
        break;
      }
    }
  }
}

int Chunk::coverHeight(int x, int z, const vector<uint8_t> &notCover) const {
  for (int y = 255; y >= 0; y--) {
    uint16_t block = blockAt(x, y, z);
    if (block == 0 || (block < notCover.size() && notCover[block])) continue;
    return y;
  }
  return -1;
}

void Chunk::reset() {
  for (int i=0; i<16; i++) {
    if (sections[i]) {
      delete sections[i];
      sections[i] = NULL;
    }
  }
}

// SPDX-License-Identifier: MPL-2.0
#include <arpa/inet.h>
#include <list>
#include <algorithm>
#include <assert.h>
#include <dirent.h>
#include <iostream>
#include <array>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <png.h>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <zlib.h>

#include "backups.h"
#include "chunk.h"
#include "event_loop.h"
#include "image.h"
#include "nbt.h"
#include "region.h"
#include "utils.h"
#include "zooms.h"

using namespace std;
using json = nlohmann::json;

static atomic<int> total_regions = 0, total_chunks = 0;
// cave layers (-c): the map as if everything above each of these y levels were cut away, see parse_region
static bool cave_layers = false;
static const int CAVE_LEVELS[] = { 64, 56, 48, 40, 32, 24, 16 };
// regions render in parallel, results shared between them are merged under this
static mutex merge_lock;

typedef struct {
  uint32_t item_search_id;
  uint32_t item_search_damage;
} searchSpec;

typedef struct {
  uint32_t id;
  uint32_t damage;
  int32_t x,y,z;
  int dim;
} possibleMatch;

// an ArchitectureCraft shape (slope, stairs...) is one block id for every material,
// the material is in its tile entity. Carpenter's Blocks do the same with a list of covers
struct CarpentersCover {
  int attribute = -1, damage = 0;
  string name;
};

struct ShapeTE {
  int32_t x = 0, y = 0, z = 0, base_data = 0, shape = -1, turn = 0, side = 0;
  string id, base_name;
  map<uint32_t, CarpentersCover> covers;
};

// "cbAttrList/<i>/<field>" inside a tile entity, the Carpenter's Blocks cover list
static CarpentersCover *carpenters_cover(ShapeTE &te, const char *field, const char *&sub) {
  static const char prefix[] = "cbAttrList/";
  if (strncmp(field, prefix, sizeof(prefix) - 1) != 0) return nullptr;
  char *end;
  uint32_t i = strtoul(field + sizeof(prefix) - 1, &end, 10);
  if (*end != '/') return nullptr;
  sub = end + 1;
  return &te.covers[i];
}

// splits "/Level/TileEntities/<n>/<field>" into n and field, false for other keys
static bool tile_entity_key(const string &key, uint32_t &index, const char *&field) {
  static const char prefix[] = "/Level/TileEntities/";
  if (key.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
  char *end;
  index = strtoul(key.c_str() + sizeof(prefix) - 1, &end, 10);
  if (*end != '/') return false;
  field = end + 1;
  return true;
}

typedef struct {
  Image *region_image;
  int xoff;
  int zoff;
  uint16_t toplayer[16][16];
  uint16_t topmeta[16][16];
  uint8_t height[16][16];
  uint16_t topshape[16][16];  // ArchitectureCraft shape of the top block, see draw_shapes_to_bitmap
  Chunk chunk;
  searchSpec *findMe;
  map<uint32_t,possibleMatch> possibleMatches;
  map<uint32_t,ShapeTE> shapes;
} chunk_parse_state;

typedef struct {
  int32_t pending_key;
  string pending_value;
  bool has_key;
  bool has_value;
  map<int32_t,string> blocks;
  map<int32_t,string> items;
} level_parse_state;

typedef struct {
  map<int32_t,string> blocks;
  map<int32_t,string> items;
  map<string, uint32_t> missing_colors;
  set<string> transparent_blocks;
  json color_table;
  // from texture-dump's blocks.json: block name -> atlas icon + 1 per meta, 0 = no texture
  map<string, array<uint16_t, 16>> texture_icons;
  // the same, resolved to this world's block ids
  vector<array<uint16_t, 16>> icons_by_id;
  map<string, int32_t> ids_by_name;
  // by block id: 1 for see-through blocks and trees (leaves, logs of any mod), which don't roof a cave
  vector<uint8_t> not_cover;
  // colors.json misses plenty of modded blocks, those get the average of their texture instead
  map<string, uint32_t> texture_colors;
} level_state;

// 1.7.10 modded level.dat contains a FML/ItemData array
// within that array is K/V pairs
// byte 0 of the K is 1 for blocks, and 2 for items
// the V is the id# for the block/item
// for chunks, /Level/Sections/${n}/Blocks is an uint8_t[16][16][16] for all blocks
// modded 1.7.10 stores bits 11:8 of the block# in /Level/Sections/${n}/Add, a uint8_t[16][16][8]
// the 12bit number from Blocks and Add is then looked up in FML/ItemData

// in 1.16, each setion has its own palette
// /sections/${n}/block_states/palette/${n}/Name has the name for each block in the palette
// /sections/${n}/block_states/palette/${n}/Properties/level has props, for blocks like logs or kelp
// /sections/${n}/block_states/data is a packed n-bit array of indexes into the pallette, inside an int64[], an index isnt split across 64bit
// so if the palette has between 16 and 31 items, it needs 5bit to represent a block
// a 64bit int can hold 12 x 5bit
// so a section has ciel((16*16*16)/12) int64's

void parseAndLoadNBT(string path, const nbt_callbacks *cb) {
  z_stream strm = {
    .total_in = 0,
    .zalloc = NULL,
    .zfree = NULL,
    .opaque = NULL,
  };
  int ret = inflateInit2(&strm, 16 + MAX_WBITS);
  assert(ret == Z_OK);
  FILE *fd = fopen(path.c_str(), "r");
  if (!fd) {
    printf("failed to open %s\n", path.c_str());
  }
  assert(fd);

  unsigned char buffer[8192];
  unsigned char *outbuf = (unsigned char*)malloc(1024*1024*16);
  strm.next_out = outbuf;
  strm.avail_out = 1024 * 1024 * 16;
  strm.total_out = 0;
  while (size_t bytes = fread(buffer, 1, 8192, fd)) {
    strm.next_in = buffer;
    strm.avail_in = bytes;
    strm.total_in += bytes;
    int flags = 0;
    if (bytes < 8192) {
      flags |= Z_FINISH;
    }
    if (!quiet) printf("%ld %d %ld %d\n", bytes, strm.avail_out, strm.total_out, strm.avail_in);
    ret = inflate(&strm, flags);
    if (!quiet) printf("%ld %d %ld %d %d\n", bytes, strm.avail_out, strm.total_out, strm.avail_in, ret);
    if (ret < 0) {
      printf("%s\n", strm.msg);
      break;
    }
    if (bytes < 8192) break;
  }
  fclose(fd);

  parseNBT(outbuf, strm.total_out, cb, false);
  free(outbuf);
  inflateEnd(&strm);
}

int get_item_index(string key) {
  const char *prefix = "/FML/ItemData/";
  const int prefix_len = strlen(prefix);
  const char *input = key.c_str();

  if (strncmp(input, prefix, prefix_len) == 0) {
    input = input + prefix_len;
    return strtol(input, NULL, 10);
  } else return -1;
}

void check_key(level_parse_state *s) {
  if (s->has_key && s->has_value) {
    int32_t key = s->pending_key;
    string value = s->pending_value;
    s->has_key = s->has_value = false;
    if (value[0] == 1) { // block
      string v = value.substr(1);
      s->blocks[key] = v;
    } else if (value[0] == 2) { // item
      string v = value.substr(1);
      s->items[key] = v;
    }
  }
}

void level_tag3(const nbt_callbacks *cb, const string key, int32_t value) {
  int index = get_item_index(key);
  if (index == -1) return;
  //printf("tag3 %s %d\n", key.c_str(), value);
  level_parse_state *s = (level_parse_state*)cb->state;

  s->has_key = true;
  s->pending_key = value;
  check_key(s);
}

void level_tag8(const nbt_callbacks *cb, const string key, const string value) {
  int index = get_item_index(key);
  if (index == -1) return;
  //printf("tag8 %s %s\n", key.c_str(), value.c_str());
  level_parse_state *s = (level_parse_state*)cb->state;

  s->has_value = true;
  s->pending_value = value;
  check_key(s);
}

void parseLevel(string root, level_state &state_out) {
  string fname = root + "/level.dat";
  level_parse_state s = {
    .has_key = false,
    .has_value = false,
  };
  nbt_callbacks level_cb = {
    .state = &s,
  };
  level_cb.tag3 = level_tag3;
  level_cb.tag8 = level_tag8;
  parseAndLoadNBT(fname, &level_cb);

  for (const auto &n : s.blocks) {
    uint32_t blockid = n.first;
    string block_name = n.second;
    //printf("%d %s\n", blockid, block_name.c_str());
  }
  for (const auto &n : s.items) {
    uint32_t itemid = n.first;
    string item_name = n.second;
    if (!quiet) printf("%d %s\n", itemid, item_name.c_str());
  }
  state_out.blocks = s.blocks;
  state_out.items = s.items;
}

static void fill_entire_chunk(Image &img, int xstart, int ystart, uint32_t color) {
  for (int pngy=0; pngy < 16; pngy++) {
    for (int pngx=0; pngx < 16; pngx++) {
      img.setPixel(xstart + pngx, ystart + pngy, color);
    }
  }
}

uint32_t swap_color(uint32_t color) {
  uint32_t r = (color >> 16) & 0xff;
  uint32_t g = (color >> 8) & 0xff;
  uint32_t b = color & 0xff;

  return (b << 16) | (g << 8) | r;
}

void draw_top_layer_to_bitmap(uint16_t toplayer[16][16], int xstart, int zstart, Image &region_image, const level_state &lstate, map<string, uint32_t> &missing_colors, int regionx, int regionz) {
  for (int z=0; z<16; z++) {
    for (int x=0; x<16; x++) {
      uint16_t block = toplayer[x][z];
      // nothing in this column (a cave layer over solid rock, the void), leave it transparent
      if (block == 0) {
        region_image.setPixel(xstart + x, zstart + z, 0);
        continue;
      }
      auto i = lstate.blocks.find(block);
      string block_name;
      if (i != lstate.blocks.end()) block_name = i->second;
      else block_name = "unknown";


      auto color = lstate.color_table.find(block_name);
      uint32_t raw_color = ( ((xstart + x) & 0xff) << 8) | ( ((zstart + z) & 0xff) << 0);
      auto texture_color = lstate.texture_colors.find(block_name);
      if (color != lstate.color_table.end()) raw_color = swap_color(*color);
      else if (texture_color != lstate.texture_colors.end()) raw_color = swap_color(texture_color->second);
      else {
        //printf("chunk xz %d %d, %d %d, %d, %s\n", x, z, xstart+x + (regionx<<9), zstart+z + (regionz << 9), block, block_name.c_str());

        missing_colors[block_name]++;
      }

      raw_color = 0xff000000 | raw_color;

      region_image.setPixel(xstart + x, zstart + z, raw_color);
    }
  }
}

void tag2_draw_map(const nbt_callbacks *cb, const std::string key, uint16_t value) {
  chunk_parse_state *s = (chunk_parse_state*)cb->state;
  {
    uint32_t te;
    const char *field, *sub;
    if (tile_entity_key(key, te, field)) {
      if (CarpentersCover *c = carpenters_cover(s->shapes[te], field, sub); c && !strcmp(sub, "Damage")) c->damage = value;
    }
  }
  //printf("%s == %d\n", key.c_str(), value);
  // only the ae2 item search needs this, and the regex is far too slow to run on every tag
  if (!s->findMe || !key.starts_with("/Level/TileEntities/")) return;
  static const regex ae2_upgrade_patt("^/Level/TileEntities/([0-9]+)/extra:[0-9]+/upgrades/#[0-9]+/(id|Damage)$");
  smatch matches;
  if (regex_search(key, matches, ae2_upgrade_patt)) {
    uint32_t te_index = atoi(matches[1].str().c_str());
    string key = matches[2];
    if (key == "id") {
      s->possibleMatches[te_index].id = value;
    } else if (key == "Damage") {
      s->possibleMatches[te_index].damage = value;
    }
  }
}

void tag1_draw_map(const nbt_callbacks *cb, const std::string key, uint8_t value) {
  chunk_parse_state *s = (chunk_parse_state*)cb->state;
  uint32_t te;
  const char *field;
  if (!tile_entity_key(key, te, field)) return;
  const char *sub;
  if (!strcmp(field, "turn")) s->shapes[te].turn = value;
  else if (!strcmp(field, "side")) s->shapes[te].side = value;
  else if (CarpentersCover *c = carpenters_cover(s->shapes[te], field, sub); c && !strcmp(sub, "cbAttribute")) c->attribute = value;
}

void tag3_draw_map(const nbt_callbacks *cb, const std::string key, int32_t value) {
  //printf("%s == %d\n", key.c_str(), value);
  chunk_parse_state *s = (chunk_parse_state*)cb->state;
  uint32_t te;
  const char *field;
  if (tile_entity_key(key, te, field)) {
    if (!strcmp(field, "x")) s->shapes[te].x = value;
    else if (!strcmp(field, "y")) s->shapes[te].y = value;
    else if (!strcmp(field, "z")) s->shapes[te].z = value;
    else if (!strcmp(field, "BaseData")) s->shapes[te].base_data = value;
    else if (!strcmp(field, "Shape")) s->shapes[te].shape = value;
  }
  if (!s->findMe || !key.starts_with("/Level/TileEntities/")) return;
  static const regex te_coord_patt("^/Level/TileEntities/([0-9]+)/(x|y|z)$");
  smatch matches;

  if (regex_search(key, matches, te_coord_patt)) {
    uint32_t te_index = atoi(matches[1].str().c_str());
    string key = matches[2];
    if (key == "x") {
      s->possibleMatches[te_index].x = value;
    } else if (key == "y") {
      s->possibleMatches[te_index].y = value;
    } else if (key == "z") {
      s->possibleMatches[te_index].z = value;
    }
  }
}

void tag7_draw_map(const nbt_callbacks *cb, string key, int size, const uint8_t *data) {
  chunk_parse_state *s = (chunk_parse_state*)cb->state;
  //printf("got map data for %s, %d bytes\n", key.c_str(), size);

  const char *prefix = "/Level/Sections/";
  const int prefix_len = strlen(prefix);

  if (strncmp(key.c_str(), prefix, prefix_len) == 0) {
    const char *num = key.c_str() + prefix_len;
    char *remain;
    uint32_t section = strtol(num, &remain, 10);
    if (strcmp(remain, "/Blocks") == 0) {
      //printf("found blocks for section %d, len %d\n", section, size);
      s->chunk.updateBlocks(section, (const uint8_t(*)[16][16])data);
    } else if (strcmp(remain, "/Add") == 0) {
      //printf("found /Add for section %d, len %d\n", section, size);
      s->chunk.updateAddBlocks(section, (const uint8_t(*)[16][8])data);
    } else if (strcmp(remain, "/Data") == 0 && size == 2048) {
      s->chunk.updateData(section, (const uint8_t(*)[16][8])data);
    } else if (strcmp(remain, "/Data16") == 0 && size == 8192) {
      s->chunk.updateData16(section, (const uint16_t(*)[16][16])data);
    } else if (strcmp(remain, "/Blocks16") == 0) {
      //printf("found blocks16 for section %d, len %d\n", section, size);
      s->chunk.updateBlocks16(section, (const uint16_t(*)[16][16])data);
    } else {
      //printf("rem %s %d\n", remain, size);
    }
  }
}

void tag8_draw_map(const nbt_callbacks *cb, const std::string key, const std::string value) {
  chunk_parse_state *s = (chunk_parse_state*)cb->state;
  uint32_t te;
  const char *field;
  if (tile_entity_key(key, te, field)) {
    const char *sub;
    if (!strcmp(field, "id")) s->shapes[te].id = value;
    else if (!strcmp(field, "BaseName")) s->shapes[te].base_name = value;
    else if (CarpentersCover *c = carpenters_cover(s->shapes[te], field, sub); c && !strcmp(sub, "cbUniqueId")) c->name = value;
    return;
  }

  const char *prefix = "/sections/";
  const int prefix_len = strlen(prefix);

  if (strncmp(key.c_str(), prefix, prefix_len) == 0) {
    const char *num = key.c_str() + prefix_len;
    char *remain;
    uint32_t section = strtol(num, &remain, 10);
    printf("%s is section %d data, %s\n", key.c_str(), section, remain);
  } else {
    //printf("%s unhandled\n", key.c_str());
  }

  //printf("%s == %s\n", key.c_str(), value.c_str());
}

// heights are a parallel tile tree under height/, y in the red channel, alpha 0 where nothing was rendered
// the zoom levels average them like colors, so a viewer divides red by alpha to undo the edge falloff
static void draw_height_to_bitmap(uint8_t height[16][16], uint16_t toplayer[16][16], int xstart, int zstart, Image &img) {
  for (int z=0; z<16; z++) {
    for (int x=0; x<16; x++) {
      img.setPixel(xstart + x, zstart + z, toplayer[x][z] ? 0xff000000 | height[x][z] : 0);
    }
  }
}

// an ArchitectureCraft shape on top of a column is drawn as the block it's made of
static void draw_shapes_as_base(chunk_parse_state &s, const level_state &lstate) {
  memset(s.topshape, 0, sizeof(s.topshape));
  for (auto &[i, te] : s.shapes) {
    // a Carpenter's block looks like its main cover (attribute 6), the frame texture is just bare wood
    if (te.id == "TileEntityCarpentersBlock") {
      for (auto &[n, c] : te.covers) {
        if (c.attribute == 6 && !c.name.empty()) {
          te.base_name = c.name;
          te.base_data = c.damage;
        }
      }
      te.shape = -1;
    } else if (te.id != "gcewing.shape") {
      continue;
    }
    if (te.base_name.empty()) continue;
    int lx = te.x & 15, lz = te.z & 15;
    if (te.y != s.height[lx][lz]) continue;
    auto base = lstate.ids_by_name.find(te.base_name);
    if (base == lstate.ids_by_name.end()) continue;
    s.toplayer[lx][lz] = base->second;
    s.topmeta[lx][lz] = te.base_data;
    // only shapes standing on the ground, the upside down and wall mounted ones still look like full blocks
    if (te.shape >= 0 && te.shape < 255 && te.side == 0) s.topshape[lx][lz] = (te.shape + 1) | ((te.turn & 3) << 8);
  }
}

// shapes are a fourth tile tree under shapes/, only at zoom 17: shape id + 1 in red, its turn
// (quarter turns around y) in green, 0 for an ordinary full block
static bool draw_shapes_to_bitmap(uint16_t topshape[16][16], int xstart, int zstart, Image &img) {
  bool any = false;
  for (int z=0; z<16; z++) {
    for (int x=0; x<16; x++) {
      img.setPixel(xstart + x, zstart + z, 0xff000000 | topshape[x][z]);
      any |= topshape[x][z] != 0;
    }
  }
  return any;
}

// textures are a third tile tree under icons/, only at zoom 17: the atlas icon + 1 of each top block
// in red (low byte) and green (high byte), 0 where there is no texture. averaging these would be
// meaningless, so there are no zoomed out levels, viewers use the color tiles there
static void draw_icons_to_bitmap(uint16_t toplayer[16][16], uint16_t topmeta[16][16], int xstart, int zstart, Image &img, const level_state &lstate) {
  for (int z=0; z<16; z++) {
    for (int x=0; x<16; x++) {
      uint16_t block = toplayer[x][z];
      uint16_t icon = block < lstate.icons_by_id.size() ? lstate.icons_by_id[block][topmeta[x][z] & 15] : 0;
      img.setPixel(xstart + x, zstart + z, 0xff000000 | icon);
    }
  }
}

// blocks.json has every block name the client knew, the world's level.dat maps them to its ids
static void resolve_icons(level_state &lstate) {
  lstate.ids_by_name.clear();
  lstate.not_cover.clear();
  for (auto &b : lstate.blocks) {
    lstate.ids_by_name[b.second] = b.first;
    if (b.first < 0) continue;
    string lower = b.second;
    for (char &c : lower) c = tolower(c);
    bool tree = lower.find("leaves") != string::npos || lower.find("log") != string::npos ||
                lower.find("tree") != string::npos || lower.find("sapling") != string::npos;
    if ((size_t)b.first >= lstate.not_cover.size()) lstate.not_cover.resize(b.first + 1, 0);
    lstate.not_cover[b.first] = tree || lstate.transparent_blocks.count(b.second);
  }
  lstate.icons_by_id.clear();
  if (lstate.texture_icons.empty()) return;
  for (auto &b : lstate.blocks) {
    auto t = lstate.texture_icons.find(b.second);
    if (t == lstate.texture_icons.end() || b.first < 0) continue;
    if ((size_t)b.first >= lstate.icons_by_id.size()) lstate.icons_by_id.resize(b.first + 1, {});
    lstate.icons_by_id[b.first] = t->second;
  }
}

// average opaque color of every 16x16 icon in the atlas
static vector<uint32_t> icon_averages(const filesystem::path &atlas_png, int columns) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, atlas_png.c_str())) return {};
  image.format = PNG_FORMAT_RGBA;
  vector<uint8_t> px(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, NULL, px.data(), 0, NULL)) return {};

  int count = columns * (image.height / 16);
  vector<uint32_t> avg(count);
  for (int i = 0; i < count; i++) {
    int ox = (i % columns) * 16, oy = (i / columns) * 16;
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < 16; y++) {
      for (int x = 0; x < 16; x++) {
        const uint8_t *p = &px[((oy + y) * image.width + ox + x) * 4];
        if (p[3] < 128) continue;
        r += p[0]; g += p[1]; b += p[2]; n++;
      }
    }
    if (n) avg[i] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
  }
  return avg;
}

static void load_textures(const filesystem::path &dir, level_state &lstate) {
  json j = json::parse(readFile(dir / "blocks.json"));
  vector<uint32_t> averages = icon_averages(dir / "atlas.png", j["columns"].get<int>());
  for (auto &[name, block] : j["blocks"].items()) {
    // -1 is drawn entirely by a tile entity renderer, its icon is meaningless
    if (block["render"].get<int>() == -1) continue;
    array<uint16_t, 16> icons{};
    for (auto &[meta, m] : block["metas"].items()) {
      int i = atoi(meta.c_str());
      if (i >= 0 && i < 16) icons[i] = m["icon"].get<int>() + 1;
    }
    lstate.texture_icons[name] = icons;

    // meta 0's texture times its tint, in colors.json's 0xRRGGBB
    auto first = block["metas"].begin();
    size_t icon = (*first)["icon"].get<int>();
    if (icon < averages.size() && averages[icon]) {
      uint32_t a = averages[icon], t = (*first)["tint"].get<uint32_t>();
      uint32_t c = 0;
      for (int shift : { 16, 8, 0 }) c |= (((a >> shift) & 0xff) * ((t >> shift) & 0xff) / 255) << shift;
      lstate.texture_colors[name] = c;
    }
  }
  printf("loaded textures for %zu blocks from %s\n", lstate.texture_icons.size(), dir.c_str());
}

filesystem::path make_png_name(filesystem::path outpath, int x, int z) {
  char outname[64];
  snprintf(outname, 60, "%d,%d.png", x, z);

  return outpath.append("17").append(outname);
}

void parse_region(const filesystem::path &region_path, int dim, signed int x, signed int z, level_state &lstate, const filesystem::path &outpath, searchSpec *findMe, list<possibleMatch> &found_blocks) {
  filesystem::path png_out = make_png_name(outpath, x, z);
  const bool verbose = false;

#if 0
  if ((x != 0) || (z != 0)) {
    puts("skipping region");
    return;
  }
#endif

  bool do_update = need_update(region_path, png_out);

  if (!do_update) {
    if (!quiet) puts("region older, skipping");
    return;
  }
  if (!quiet) puts("updating");

  Image region_image;
  Image height_image;
  Image icon_image;
  Image shape_image;
  bool any_shapes = false;
  struct CaveLayer {
    int y;
    Image color, height, icons;
    bool any = false;
  };
  vector<CaveLayer> caves(cave_layers ? size(CAVE_LEVELS) : 0);
  for (size_t i = 0; i < caves.size(); i++) {
    caves[i].y = CAVE_LEVELS[i];
    for (Image *img : { &caves[i].color, &caves[i].height, &caves[i].icons }) memset(img->bitmap, 0, 512 * 512 * 4);
  }
  const bool textured = !lstate.icons_by_id.empty();
  map<string, uint32_t> missing_colors;
  list<possibleMatch> found_here;
  memset(region_image.bitmap, 0, 512 * 512 * 4);
  memset(height_image.bitmap, 0, 512 * 512 * 4);
  memset(icon_image.bitmap, 0, 512 * 512 * 4);
  memset(shape_image.bitmap, 0, 512 * 512 * 4);

  FILE *fd = fopen(region_path.c_str(), "r");
  fseek(fd, 0, SEEK_END);
  uint64_t region_size_bytes = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  if (region_size_bytes == 0) {
    fclose(fd);
    return;
  }

  total_regions++;

  region_header *header = (region_header*)malloc(sizeof(*header));
  fread(header, sizeof(*header), 1, fd);

  chunk_parse_state s = {
    .region_image = &region_image,
    .findMe = findMe,
  };
  nbt_callbacks chunk_callbacks = {
    .state = &s,
  };
  chunk_callbacks.tag2 = &tag2_draw_map;
  chunk_callbacks.tag1 = &tag1_draw_map;
  chunk_callbacks.tag3 = &tag3_draw_map;
  chunk_callbacks.tag7 = &tag7_draw_map;
  chunk_callbacks.tag8 = &tag8_draw_map;

  for (int xoff = 0; xoff < 32; xoff++) {
    for (int zoff = 0; zoff < 32; zoff++) {
      s.xoff = xoff;
      s.zoff = zoff;

      //if (xoff != 28) continue;
      //if (zoff != 16) continue;

      s.chunk.reset();

      //fill_entire_chunk(row_pointers, xoff << 4, zoff << 4, 255, 255, 255);

      uint32_t offset_raw = ntohl(header->chunk_offsets[zoff][xoff]);
      uint32_t offset = (offset_raw >> 8) * 4096;
      uint32_t length = (offset_raw & 0xff) * 4096;

      int xpos = (x << 9) | (xoff << 4);
      int zpos = (z << 9) | (zoff << 4);
      if (offset == 0) {
        int png_xstart = xoff << 4;
        int png_zstart = zoff << 4;
        fill_entire_chunk(region_image, png_xstart, png_zstart, 0);
        continue;
      }

      total_chunks++;

      //snprintf(outname, 60, "chunks/%d,%d.zlib", (x<<5) | xoff, (z<<5) | zoff);

      uint32_t lastmod = ntohl(header->chunk_lastmods[zoff][xoff]);
      string name = region_path.filename().string();
      if (verbose) printf("%s %d %d, x %d<->%d z %d<->%d == off %d sz %d %d\n", name.c_str(), xoff, zoff, xpos, xpos+15, zpos, zpos+15, offset, length, lastmod);

      void *raw_chunk = malloc(length);
      fseek(fd, offset, SEEK_SET);
      int got = fread(raw_chunk, length, 1, fd);
      assert(got == 1);
      //printf("%d %d 0x%x\n", got, length);

      uint32_t actual_length = ntohl(*(uint32_t*)raw_chunk);
      uint8_t comp_type = *(uint8_t*)(raw_chunk+4);
      //printf("length: %d, comp: %d\n", actual_length, comp_type);
      assert(comp_type == 2); // TODO, support other compression types

      //printf("dd if=\"%s\" of=extract bs=1 count=%d skip=%d\n", path.c_str(), actual_length-1, offset+5);

      const int uncomp_size = 1024*1024 * 3;
      void *uncompressed = malloc(uncomp_size);
      //memset(uncompressed, 0x55, uncomp_size);

      //FILE *chunkout = fopen(outname, "wb");
      //fwrite(raw_chunk+5, actual_length-1, 1, chunkout);
      //fclose(chunkout);

      int uncompressed_size = decompress(raw_chunk + 5, actual_length-1, uncompressed, uncomp_size);
      //printf("true size: %d\n", uncompressed_size);

      if (uncompressed_size > 0) {
        parseNBT((const unsigned char *)uncompressed, uncompressed_size, &chunk_callbacks, false);
        fill_entire_chunk(region_image, xoff << 4, zoff << 4, 0);
        // the nether's bedrock roof spans y 123-127 with pockets in it, start under it
        if (dim == -1) s.chunk.getTopMostBlocks(s.toplayer, s.topmeta, s.height, lstate.transparent_blocks, lstate.blocks, 122, true);
        else s.chunk.getTopMostBlocks(s.toplayer, s.topmeta, s.height, lstate.transparent_blocks, lstate.blocks);
        draw_shapes_as_base(s, lstate);
        if (draw_shapes_to_bitmap(s.topshape, xoff << 4, zoff << 4, shape_image)) any_shapes = true;
        if (textured) draw_icons_to_bitmap(s.toplayer, s.topmeta, xoff << 4, zoff << 4, icon_image, lstate);
        draw_top_layer_to_bitmap(s.toplayer, xoff << 4, zoff << 4, region_image, lstate, missing_colors, x, z);
        draw_height_to_bitmap(s.height, s.toplayer, xoff << 4, zoff << 4, height_image);

        // the same again for every cave layer, ignoring shapes, they're nearly all on the surface
        int cover[16][16];
        if (!caves.empty())
          for (int cx = 0; cx < 16; cx++)
            for (int cz = 0; cz < 16; cz++) cover[cx][cz] = s.chunk.coverHeight(cx, cz, lstate.not_cover);
        for (auto &cave : caves) {
          uint16_t toplayer[16][16], topmeta[16][16];
          uint8_t height[16][16];
          s.chunk.getTopMostBlocks(toplayer, topmeta, height, lstate.transparent_blocks, lstate.blocks, cave.y, true);
          // only floors with something solid over them are caves, not the open ground under trees
          for (int cx = 0; cx < 16; cx++)
            for (int cz = 0; cz < 16; cz++)
              if (toplayer[cx][cz] && height[cx][cz] >= cover[cx][cz]) toplayer[cx][cz] = 0;
          if (textured) draw_icons_to_bitmap(toplayer, topmeta, xoff << 4, zoff << 4, cave.icons, lstate);
          draw_top_layer_to_bitmap(toplayer, xoff << 4, zoff << 4, cave.color, lstate, missing_colors, x, z);
          // solid rock all the way down sits at the cut, so in 3d caves are hollows in a slab
          for (int cx = 0; cx < 16; cx++)
            for (int cz = 0; cz < 16; cz++)
              cave.height.setPixel((xoff << 4) + cx, (zoff << 4) + cz, 0xff000000 | (toplayer[cx][cz] ? height[cx][cz] : cave.y));
          cave.any = true;
        }
        if (findMe) {
          for (auto pm : s.possibleMatches) {
            if (pm.second.id != findMe->item_search_id) continue;
            if (pm.second.damage != findMe->item_search_damage) continue;
            pm.second.dim = dim;
            found_here.push_back(pm.second);
          }
        }
        s.possibleMatches.clear();
        s.shapes.clear();
        //if ((xoff == 28) && (zoff == 16)) {
        //  s.chunk.printSection(3);
        //  s.chunk.printSection(15);
        //}
        //snprintf(outname, 60, "chunks/%d,%d.good", (x<<5) | xoff, (z<<5) | zoff);
        //to_file(outname, uncompressed, uncompressed_size);
      } else {
        char outname[256];
        fill_entire_chunk(region_image, xoff << 4, zoff << 4, 0xff0000ff);
        snprintf(outname, 60, "chunks/%d,%d.bad", (x<<5) | xoff, (z<<5) | zoff);
        to_file(outname, uncompressed, uncomp_size);
      }

      free(raw_chunk);
      free(uncompressed);
    }
  }

  //puts("writing png");
  write_png(png_out, region_image);
  write_png(make_png_name(outpath / "height", x, z), height_image);
  if (textured) write_png(make_png_name(outpath / "icons", x, z), icon_image);
  // most regions have no shapes at all, a missing shape tile means every block is full
  if (any_shapes) write_png(make_png_name(outpath / "shapes", x, z), shape_image);
  for (auto &cave : caves) {
    if (!cave.any) continue;
    filesystem::path dir = outpath / "caves" / to_string(cave.y);
    write_png(make_png_name(dir, x, z), cave.color);
    write_png(make_png_name(dir / "height", x, z), cave.height);
    if (textured) write_png(make_png_name(dir / "icons", x, z), cave.icons);
  }

  {
    lock_guard<mutex> l(merge_lock);
    for (auto &m : missing_colors) lstate.missing_colors[m.first] += m.second;
    found_blocks.splice(found_blocks.end(), found_here);
  }

  free(header);
  fclose(fd);
}

void region_loop(const filesystem::path &path, int dim, level_state &lstate, const filesystem::path &outpath, searchSpec *findMe, list<possibleMatch> &foundBlocks) {
  struct Job { filesystem::path p; int x, z; };
  vector<Job> jobs;
  for (auto &ent : filesystem::directory_iterator{path}) {
    auto &p = ent.path();
    if (!ent.is_directory()) {
      string filename = p.stem().string();
      string no_r_name = filename.substr(2);
      size_t split_pos = no_r_name.find(".");
      if (split_pos != string::npos) {
        string x_coord = no_r_name.substr(0, split_pos);
        int x = atoi(x_coord.c_str());
        string z_coord = no_r_name.substr(split_pos + 1);
        int z = atoi(z_coord.c_str());

        //printf("%s == %d %d, x %d<->%d, z %d<->%d\n", filename.c_str(), x, z, x << 9, (x+1) << 9, z << 9, (z+1) << 9);
        jobs.push_back({p, x, z});
      }
    }
  }

  // regions are independent, render them on every core
  filesystem::create_directories(outpath / "17");
  filesystem::create_directories(outpath / "height" / "17");
  filesystem::create_directories(outpath / "icons" / "17");
  filesystem::create_directories(outpath / "shapes" / "17");
  for (int y : CAVE_LEVELS) {
    if (!cave_layers) break;
    filesystem::path dir = outpath / "caves" / to_string(y);
    for (auto sub : { "17", "height/17", "icons/17" }) filesystem::create_directories(dir / sub);
  }
  parallel_for(jobs.size(), [&](size_t i) {
    parse_region(jobs[i].p, dim, jobs[i].x, jobs[i].z, lstate, outpath, findMe, foundBlocks);
  });

#if 0
  DIR *fd = opendir(path.c_str());
  struct dirent *dir;
  while ((dir = readdir(fd))) {
    int x, z;
    char ext[32];
    int res = sscanf(dir->d_name, "r.%d.%d.%s", &x, &z, ext);
    if (res != 3) {
      printf("ignoring %s\n", dir->d_name);
      continue;
    }
    printf("%s == %d %d %s, x %d<->%d, z %d<->%d\n", dir->d_name, x, z, ext, x << 9, (x+1) << 9, z << 9, (z+1) << 9);
    parse_region(path, dir->d_name, x, z, lstate, outpath);
  }
  closedir(fd);
#endif
}

// renders every dimension of `savepath` into `outpath`, overworld at the root, others in DIMxx/
static void render_world(const filesystem::path &savepath, const filesystem::path &outpath, level_state &lstate, searchSpec *findMe, list<possibleMatch> &foundBlocks) {
  for_each_dimension(savepath, [outpath, &lstate, &findMe, &foundBlocks](std::filesystem::path dir, int dim) -> void {
    string dname = dir.stem();
    dir /= "region";
    filesystem::path out = outpath;
    if (dim != 0) out /= dname;
    if (!quiet) cout << dir << " " << out << endl;
    if (!exists(dir)) return;
    if (!exists(out)) create_directories(out);
    region_loop(dir, dim, lstate, out, findMe, foundBlocks);
    // a dimension whose region dir has no regions yet has nothing to zoom
    if (exists(out / "17")) regen_zooms(out, true);
    if (exists(out / "height" / "17")) regen_zooms(out / "height", true);
    for (int y : CAVE_LEVELS) {
      filesystem::path cave = out / "caves" / to_string(y);
      if (exists(cave / "17")) regen_zooms(cave, true);
      if (exists(cave / "height" / "17")) regen_zooms(cave / "height", true);
    }
  });
}

int main(int argc, char **argv) {
  char *savepath = NULL;
  std::filesystem::path outpath = ".";
  std::filesystem::path datadir = ".";
  const char *backupdir = NULL, *dbpath = NULL, *texturedir = NULL;
  bool watch_dir = false;
  bool bobby = false;

  level_state lstate;
  own_age = get_own_age();

  int opt;
  while ((opt = getopt(argc, argv, "p:o:wbz:d:j:t:c")) != -1) {
    switch (opt) {
    case 'p':
      savepath = optarg;
      break;
    case 'o':
      outpath = optarg;
      break;
    case 'w':
      watch_dir = true;
      break;
    case 'b':
      bobby = true;
      break;
    case 'z':
      backupdir = optarg;
      break;
    case 'd':
      dbpath = optarg;
      break;
    case 'j':
      datadir = optarg;
      break;
    case 't':
      texturedir = optarg;
      break;
    case 'c':
      cave_layers = true;
      break;
    }
  }

  if (!savepath && !backupdir) {
    puts("error, specify a save path with `-p <path>`, or a backup dir and tile db with `-z <dir> -d <db>`");
    return 0;
  }
  if (backupdir && !dbpath) {
    puts("error, `-z` needs a tile db, `-d <db>`");
    return 0;
  }

  // under systemd stdout is a pipe, flush every line so the journal keeps up. this has to come
  // before anything is printed
  if (backupdir) setvbuf(stdout, nullptr, _IOLBF, 0);

  lstate.color_table = json::parse(readFile(datadir / "colors.json"));
  lstate.transparent_blocks = json::parse(readFile(datadir / "transparent_blocks.json"));
  // optional, a texture-dump output directory, adds the icons/ tiles for the textured view
  if (texturedir) load_textures(texturedir, lstate);

  if (backupdir) {
    quiet = true;
    // corrupt chunks get dumped here
    filesystem::create_directories("chunks");
    backup_loop(backupdir, dbpath, [&lstate](const filesystem::path &save, const filesystem::path &out) {
      // block and item ids can change between backups, so reload them every time
      level_state backup_state = lstate;
      parseLevel(save, backup_state);
      resolve_icons(backup_state);
      list<possibleMatch> unused;
      render_world(save, out, backup_state, NULL, unused);
      lstate.missing_colors = backup_state.missing_colors;
    });
    return 0;
  }

  if (!bobby) {
    parseLevel(savepath, lstate);
    resolve_icons(lstate);
  }

  if (watch_dir) {
    event_loop(savepath, outpath);
  } else {
    searchSpec *findMe = NULL;
    if (true) {
      auto it = find_if(lstate.items.begin(), lstate.items.end(), [](pair<uint32_t,string> p) -> bool {
        return p.second == "appliedenergistics2:item.ItemMultiMaterial";
      });
      if (it != lstate.items.end()) {
        findMe = (searchSpec*)malloc(sizeof(searchSpec));
        findMe->item_search_id = it->first;
        findMe->item_search_damage = 53;
      }
    }
    list<possibleMatch> foundBlocks;
    if (bobby) {
      region_loop(string(savepath) + "/overworld/", 0, lstate, outpath, findMe, foundBlocks);
    } else {
      //region_loop(string(savepath) + "/region/", "Overworld", lstate, outpath);
      //region_loop(string(savepath) + "/PERSONAL_DIM_183/region/", lstate, outpath);
      //region_loop(string(savepath) + "DIM-1/region/", "Nether", lstate, outpath);
      render_world(savepath, outpath, lstate, findMe, foundBlocks);
    }
    for (auto pm : foundBlocks) {
      printf("matches dim %d, %d/%d/%d %d:%d\n", pm.dim, pm.x, pm.y, pm.z, pm.id, pm.damage);
    }
  }

  json obj = lstate.missing_colors;
  string obj_json = obj.dump();
  to_file("missing_colors.json", obj_json.c_str(), obj_json.length());

  printf("processed %d chunks in %d regions\n", total_chunks.load(), total_regions.load());


  return 0;
}

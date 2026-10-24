// SPDX-License-Identifier: MPL-2.0
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

static const int MAX_ZOOM = 17, MIN_ZOOM = 6, TILE_PX = 512;
static const size_t MAX_TEXTURES = 1024;
static const int MAX_INFLIGHT = 8;
// 3d level of detail, in tile pixels from the orbit target: real block steps up close (zoom 17 only),
// then a smooth surface with a vertex per pixel, then 4 pixels per quad. tiles are cut into 16x16
// patches of 32 pixels wherever any of that detail is needed
static const int PATCHES = 16, PATCH_PX = TILE_PX / PATCHES;
// the 3d always uses full resolution tiles. block steps reach out to `blocky_r` (it grows with the camera
// distance), then full resolution smooth patches to `full_r`, then whole tiles
static double blocky_r = 160, full_r = 320;
// layer 0 is the surface, the rest are biome-thingy's cave layers (-c), cut at these y levels
static const int CAVE_LEVELS[] = { 0, 64, 56, 48, 40, 32, 24, 16 };
static const int LAYERS = 8;
static const double FOV = 50 * M_PI / 180;
static const float BG[3] = { 0, 0, 0 };

enum TileState { LOADING, LOADED, MISSING };
enum Kind { COLOR = 0, HEIGHT = 1, ICONS = 2, SHAPES = 3 };

struct Tile {
  GLuint tex = 0;
  TileState state = LOADING;
  uint32_t last_used = 0;
};

struct Want {
  uint64_t key;
  double priority;
  int kind, layer, zoom, x, y;
};

static struct {
  int width = 1, height = 1;
  bool mode3d = false;
  double cx = 0, cz = 0;      // block at the center of the screen (2d), or the orbit target (3d)
  double scale = 0.5;         // 2d: screen pixels per block
  double yaw = 0, pitch = 0.8, dist = 600, ty = 64;  // 3d orbit camera around (cx, ty, cz)
  string dim;                 // "" for the overworld, "DIM-1" etc
  int layer = 0;              // 2d: the surface (0) or a cave layer, see CAVE_LEVELS
  int pass = 0;               // the layer being fetched and drawn right now
  float xray = 1;             // 3d: opacity of the surface, below 1 the caves under it show through
  int sid = 0, prev_sid = 0;  // prev_sid stays on screen until the new snapshot's tiles arrive
  bool dirty = true;
  uint32_t frame = 0;
  int inflight = 0;
  int next_request = 0;
  unordered_map<int, pair<uint64_t, GLuint>> requests;  // fetch id -> tile key and texture, js numbers cannot hold the 64 bit keys
  unordered_map<uint64_t, Tile> tiles;
  vector<Want> wanted;  // rebuilt every frame

  GLuint prog2d, quad;
  GLint u_rect, u_tex;

  bool can3d = false;
  struct Mesh { GLuint vbo = 0, ibo = 0; int count = 0; };
  // smooth grids of 128 (whole tile), 64 (coarse ring), 32 and 8 quads (patches), and the block steps
  Mesh grid128, grid64, grid32, grid8, blocky;
  struct Prog3d {
    GLuint id;
    GLint mvp, tile, sub, quads, eye, fog, color, heightmap, icons, atlas, tint, atlas_size, tint_size, cols, textured, patch, shapes, profiles, cave;
    GLint cut, band, surface, alpha;
  } smooth, steps;
  // ArchitectureCraft top surfaces: a 3x3 height grid per shape id, and a stand in for regions without shapes
  GLuint profiles = 0, no_shapes = 0;

  // textured view, all optional: missing atlas means flat colors only
  bool textures = false;
  GLuint atlas = 0, tint = 0;
  float atlas_w, atlas_h, tint_w, tint_h, atlas_cols;
  GLuint prog_tex;
  GLint ut_rect, ut_color, ut_icons, ut_atlas, ut_tint, ut_atlas_size, ut_tint_size, ut_fade, ut_cols;

  bool dragging = false, rotating = false;
  double drag_x, drag_y;
  double pinch_dist = 0, pinch_angle = 0, pinch_y = 0;
} V;

static uint64_t tile_key(int kind, int layer, int sid, int zoom, int x, int y) {
  return ((uint64_t)(uint32_t)sid << 44) | ((uint64_t)layer << 41) | ((uint64_t)kind << 39) | ((uint64_t)zoom << 34) |
         ((uint64_t)(uint32_t)(x & 0x1ffff) << 17) | (uint32_t)(y & 0x1ffff);
}

// fetch and decode with the browser, then upload straight into the texture
// no premultiplying or color conversion, the height tiles are data and not a picture
EM_JS(void, js_load_tile, (const char *url, int id, int tex, int mipmap), {
  fetch(UTF8ToString(url))
    .then(r => { if (!r.ok) throw r.status; return r.blob(); })
    .then(b => createImageBitmap(b, { premultiplyAlpha: "none", colorSpaceConversion: "none" }))
    .then(bmp => {
      GLctx.bindTexture(GLctx.TEXTURE_2D, GL.textures[tex]);
      GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, bmp);
      if (mipmap) GLctx.generateMipmap(GLctx.TEXTURE_2D);
      bmp.close();
      _viewer_tile_done(id, 1);
    })
    .catch(() => _viewer_tile_done(id, 0));
});

// the atlas goes into `atlas`, and blocks.json becomes a table of one tint per icon in `tint`
// (blocks sharing an icon nearly always share the tint too), laid out 256 icons per row
EM_JS(void, js_load_textures, (int atlas, int tint), {
  Promise.all([
    fetch("textures/atlas.png").then(r => { if (!r.ok) throw r.status; return r.blob(); })
      .then(b => createImageBitmap(b, { premultiplyAlpha: "none", colorSpaceConversion: "none" })),
    fetch("textures/blocks.json").then(r => { if (!r.ok) throw r.status; return r.json(); }),
  ]).then(([bmp, info]) => {
    let icons = 0;
    for (const b of Object.values(info.blocks))
      for (const m of Object.values(b.metas)) icons = Math.max(icons, m.icon + 1);
    const tw = 256, th = Math.max(1, Math.ceil(icons / tw));
    const table = new Uint8Array(tw * th * 4).fill(255);
    for (const b of Object.values(info.blocks))
      for (const m of Object.values(b.metas)) {
        const o = m.icon * 4;
        table[o] = (m.tint >> 16) & 255; table[o + 1] = (m.tint >> 8) & 255; table[o + 2] = m.tint & 255;
      }
    GLctx.bindTexture(GLctx.TEXTURE_2D, GL.textures[atlas]);
    GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, bmp);
    GLctx.bindTexture(GLctx.TEXTURE_2D, GL.textures[tint]);
    GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, tw, th, 0, GLctx.RGBA, GLctx.UNSIGNED_BYTE, table);
    _viewer_textures_ready(bmp.width, bmp.height, tw, th, info.columns);
    bmp.close();
  }).catch(e => console.log("no block textures, flat colors only", e));
});

EM_JS(void, js_fail, (int code), {
  if (Module.onFail) Module.onFail("WebGL is not available in this browser (error " + code + "). Check that hardware acceleration is on.");
});

// main runs after emscripten's onRuntimeInitialized, so the page waits for this before touching the viewer
EM_JS(void, js_ready, (), {
  if (Module.onViewerReady) Module.onViewerReady();
});

EM_JS(void, js_show_cursor, (double x, double z), {
  if (Module.onCursor) Module.onCursor(x, z);
});

extern "C" EMSCRIPTEN_KEEPALIVE void viewer_tile_done(int id, int ok) {
  V.inflight--;
  auto req = V.requests.find(id);
  if (req == V.requests.end()) return;
  auto [key, tex] = req->second;
  V.requests.erase(req);
  auto it = V.tiles.find(key);
  if (it == V.tiles.end() || it->second.tex != tex) {
    // the dimension changed while loading, clear_tiles left the texture to us
    glDeleteTextures(1, &tex);
    return;
  }
  it->second.state = ok ? LOADED : MISSING;
  if (ok) V.dirty = true;
}

extern "C" EMSCRIPTEN_KEEPALIVE void viewer_textures_ready(int aw, int ah, int tw, int th, int cols) {
  V.atlas_w = aw; V.atlas_h = ah; V.tint_w = tw; V.tint_h = th; V.atlas_cols = cols;
  V.textures = true;
  V.dirty = true;
}

static void request_tile(const Want &w) {
  Tile t;
  glGenTextures(1, &t.tex);
  glBindTexture(GL_TEXTURE_2D, t.tex);
  if (w.kind == COLOR) {
    // mipmaps keep distant terrain from shimmering in 3d
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    // blocks should stay crisp squares when zoomed in past 1:1
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  } else {
    // heights are sampled in the vertex shader, which webgl 1 only allows unfiltered,
    // and icon indices must never be blended
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  t.last_used = V.frame;
  V.tiles[w.key] = t;

  char url[256];
  char layer[32] = "";
  if (w.layer) snprintf(layer, sizeof(layer), "caves/%d/", CAVE_LEVELS[w.layer]);
  snprintf(url, sizeof(url), "tile/%d/%s%s%s%s%d/%d,%d.png", V.sid, V.dim.c_str(), V.dim.empty() ? "" : "/",
           layer, w.kind == HEIGHT ? "height/" : w.kind == ICONS ? "icons/" : w.kind == SHAPES ? "shapes/" : "", w.zoom, w.x, w.y);
  V.inflight++;
  int id = V.next_request++;
  V.requests[id] = {w.key, t.tex};
  js_load_tile(url, id, t.tex, w.kind == COLOR);
}

static void free_tile(Tile &t) {
  if (t.tex) glDeleteTextures(1, &t.tex);
}

static void clear_tiles() {
  for (auto &kv : V.tiles) {
    // in flight fetches still upload into their texture, it's freed when they finish
    if (kv.second.state != LOADING) free_tile(kv.second);
  }
  V.tiles.clear();
}

static void evict() {
  if (V.tiles.size() <= MAX_TEXTURES) return;
  vector<pair<uint32_t, uint64_t>> age;
  for (auto &kv : V.tiles) {
    // in flight textures still get written by the fetch, leave them
    if (kv.second.state != LOADING && kv.second.last_used != V.frame) age.push_back({kv.second.last_used, kv.first});
  }
  sort(age.begin(), age.end());
  size_t drop = min(age.size(), V.tiles.size() - MAX_TEXTURES);
  for (size_t i = 0; i < drop; i++) {
    free_tile(V.tiles[age[i].second]);
    V.tiles.erase(age[i].second);
  }
}

// the level whose pixels are closest to (but not smaller than) `blocks_per_px`
static int zoom_for(double blocks_per_px) {
  int z = MAX_ZOOM - (int)floor(log2(fmax(1.0, blocks_per_px)));
  return z < MIN_ZOOM ? MIN_ZOOM : z;
}

static double tile_blocks(int zoom) {
  return (double)(TILE_PX << (MAX_ZOOM - zoom));
}

// the current snapshot's tile, or the previous snapshot's while it loads
static Tile *find_loaded(int kind, int zoom, int x, int y) {
  for (int sid : { V.sid, V.prev_sid }) {
    auto it = V.tiles.find(tile_key(kind, V.pass, sid, zoom, x, y));
    if (it != V.tiles.end() && it->second.state == LOADED) {
      it->second.last_used = V.frame;
      return &it->second;
    }
  }
  return nullptr;
}

// queue the current snapshot's tile if it isn't cached yet
static void want(int kind, int zoom, int x, int y, double priority) {
  uint64_t key = tile_key(kind, V.pass, V.sid, zoom, x, y);
  auto it = V.tiles.find(key);
  if (it == V.tiles.end()) V.wanted.push_back({key, priority, kind, V.pass, zoom, x, y});
  else it->second.last_used = V.frame;
}

// 2d

static void draw_level_2d(int zoom, bool request) {
  const double size = tile_blocks(zoom);
  const double half_w = V.width / 2.0 / V.scale, half_h = V.height / 2.0 / V.scale;
  const int x0 = (int)floor((V.cx - half_w) / size), x1 = (int)floor((V.cx + half_w) / size);
  const int y0 = (int)floor((V.cz - half_h) / size), y1 = (int)floor((V.cz + half_h) / size);

  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      if (request) {
        // nearest to the center loads first
        double dx = (x + 0.5) * size - V.cx, dy = (y + 0.5) * size - V.cz;
        want(COLOR, zoom, x, y, dx * dx + dy * dy);
      }
      Tile *t = find_loaded(COLOR, zoom, x, y);
      if (!t) continue;

      // tile rect in normalized device coordinates
      double sx0 = (x * size - V.cx) * V.scale, sx1 = sx0 + size * V.scale;
      double sy0 = (y * size - V.cz) * V.scale, sy1 = sy0 + size * V.scale;
      glUniform4f(V.u_rect, sx0 * 2 / V.width, -sy0 * 2 / V.height, sx1 * 2 / V.width, -sy1 * 2 / V.height);
      glBindTexture(GL_TEXTURE_2D, t->tex);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
}

// how much of the block textures to show, they need a few screen pixels per block to read
static float texture_fade(double px_per_block) {
  return V.textures ? (float)fmin(1.0, fmax(0.0, (px_per_block - 2.0) / 2.0)) : 0;
}

static void bind_texture_units(int first) {
  glActiveTexture(GL_TEXTURE0 + first);
  glBindTexture(GL_TEXTURE_2D, V.atlas);
  glActiveTexture(GL_TEXTURE0 + first + 1);
  glBindTexture(GL_TEXTURE_2D, V.tint);
  glActiveTexture(GL_TEXTURE0);
}

// redraws the visible zoom 17 tiles with block textures, over the flat pass
static void draw_textured_2d(float fade) {
  glUseProgram(V.prog_tex);
  glUniform1i(V.ut_color, 0);
  glUniform1i(V.ut_icons, 1);
  glUniform1i(V.ut_atlas, 2);
  glUniform1i(V.ut_tint, 3);
  glUniform2f(V.ut_atlas_size, V.atlas_w, V.atlas_h);
  glUniform2f(V.ut_tint_size, V.tint_w, V.tint_h);
  glUniform1f(V.ut_cols, V.atlas_cols);
  glUniform1f(V.ut_fade, fade);
  bind_texture_units(2);

  const double size = tile_blocks(MAX_ZOOM);
  const double half_w = V.width / 2.0 / V.scale, half_h = V.height / 2.0 / V.scale;
  const int x0 = (int)floor((V.cx - half_w) / size), x1 = (int)floor((V.cx + half_w) / size);
  const int y0 = (int)floor((V.cz - half_h) / size), y1 = (int)floor((V.cz + half_h) / size);
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      double dx = (x + 0.5) * size - V.cx, dy = (y + 0.5) * size - V.cz;
      want(ICONS, MAX_ZOOM, x, y, dx * dx + dy * dy);
      Tile *c = find_loaded(COLOR, MAX_ZOOM, x, y);
      Tile *i = find_loaded(ICONS, MAX_ZOOM, x, y);
      if (!c || !i) continue;
      double sx0 = (x * size - V.cx) * V.scale, sx1 = sx0 + size * V.scale;
      double sy0 = (y * size - V.cz) * V.scale, sy1 = sy0 + size * V.scale;
      glUniform4f(V.ut_rect, sx0 * 2 / V.width, -sy0 * 2 / V.height, sx1 * 2 / V.width, -sy1 * 2 / V.height);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, i->tex);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, c->tex);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
}

static void draw_2d() {
  V.pass = V.layer;
  glDisable(GL_DEPTH_TEST);
  glUseProgram(V.prog2d);
  glBindBuffer(GL_ARRAY_BUFFER, V.quad);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(V.u_tex, 0);

  // coarser levels underneath fill the gaps while the sharp ones load
  int zoom = zoom_for(1.0 / V.scale);
  for (int z = max(MIN_ZOOM, zoom - 4); z <= zoom; z++) draw_level_2d(z, z == zoom);

  float fade = texture_fade(V.scale);
  if (zoom == MAX_ZOOM && fade > 0) draw_textured_2d(fade);
}

// 3d

struct Mat4 { float m[16]; };  // column major

static Mat4 mul(const Mat4 &a, const Mat4 &b) {
  Mat4 r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  return r;
}

static Mat4 perspective(double fovy, double aspect, double n, double f) {
  double t = 1.0 / tan(fovy / 2);
  Mat4 r = {};
  r.m[0] = t / aspect;
  r.m[5] = t;
  r.m[10] = (f + n) / (n - f);
  r.m[11] = -1;
  r.m[14] = 2 * f * n / (n - f);
  return r;
}

// view matrix relative to the eye, positions are passed in eye-relative so floats keep their precision far from 0,0
static Mat4 look(double fx, double fy, double fz) {
  double fl = sqrt(fx * fx + fy * fy + fz * fz);
  fx /= fl; fy /= fl; fz /= fl;
  // side = forward x up(0,1,0)
  double sx = -fz, sy = 0, sz = fx;
  double sl = sqrt(sx * sx + sz * sz);
  sx /= sl; sz /= sl;
  // up = side x forward
  double ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
  Mat4 r = {};
  r.m[0] = sx; r.m[4] = sy; r.m[8] = sz;
  r.m[1] = ux; r.m[5] = uy; r.m[9] = uz;
  r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
  r.m[15] = 1;
  return r;
}

static void eye_pos(double &ex, double &ey, double &ez) {
  ex = V.cx + V.dist * cos(V.pitch) * sin(V.yaw);
  ey = V.ty + V.dist * sin(V.pitch);
  ez = V.cz + V.dist * cos(V.pitch) * cos(V.yaw);
}

// how many blocks one screen pixel covers at the orbit target
static double blocks_per_px_3d() {
  return V.dist * 2 * tan(FOV / 2) / V.height;
}

// the frame's view and eye height, to skip patches off screen
static Mat4 cull_mvp;
static double cull_ey;

// true if a box of the world (eye relative x, z, any height 0..256) is entirely off screen
static bool off_screen(double x0, double z0, double size) {
  int out[6] = {};
  const float *m = cull_mvp.m;
  for (int i = 0; i < 8; i++) {
    double x = x0 + (i & 1 ? size : 0), z = z0 + (i & 2 ? size : 0), y = (i & 4 ? 256 : 0) - cull_ey;
    double cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    double cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    double cz = m[2] * x + m[6] * y + m[10] * z + m[14];
    double cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    out[0] += cx < -cw; out[1] += cx > cw; out[2] += cy < -cw; out[3] += cy > cw; out[4] += cz < -cw; out[5] += cz > cw;
  }
  for (int k = 0; k < 6; k++)
    if (out[k] == 8) return true;
  return false;
}

struct BlockyPatch { Tile *c, *h, *icons, *shapes, *surface; float x0, z0, px, pz, textured; };

static void use_prog3d(const decltype(V.smooth) &p, const Mat4 &mvp, float ey, double reach) {
  glUseProgram(p.id);
  glUniformMatrix4fv(p.mvp, 1, GL_FALSE, mvp.m);
  glUniform1f(p.eye, ey);
  glUniform2f(p.fog, (float)(reach * 0.5), (float)reach);
  glUniform1i(p.color, 0);
  glUniform1i(p.heightmap, 1);
  glUniform1i(p.icons, 2);
  glUniform1i(p.atlas, 3);
  glUniform1i(p.tint, 4);
  glUniform2f(p.atlas_size, V.atlas_w, V.atlas_h);
  glUniform2f(p.tint_size, V.tint_w, V.tint_h);
  glUniform1f(p.cols, V.atlas_cols);
  glUniform1f(p.cave, V.pass ? 1 : 0);
  glUniform1i(p.surface, 7);
  glUniform1f(p.alpha, 1);
}

// which cave floors a layer draws: below its cut, and above the next cut down (the lowest takes the rest),
// so every floor is drawn once. floors that are really the open air surface are left to the surface
static void layer_uniforms(const decltype(V.smooth) &p) {
  glUniform1f(p.cut, V.pass ? CAVE_LEVELS[V.pass] : 0);
  glUniform1f(p.band, V.pass && V.pass < LAYERS - 1 ? CAVE_LEVELS[V.pass + 1] : 0);
  glUniform1f(p.cave, V.pass ? 1 : 0);
}

static void use_mesh(const decltype(V.grid8) &m) {
  glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
}

static void bind_tile(Tile *c, Tile *h, Tile *icons) {
  if (icons) {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, icons->tex);
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, h->tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->tex);
}

// smooth pass over one level. close patches of zoom 17 tiles are collected into `blocky` instead
// the surface height tile under a cave layer's tile, to leave out floors that are the surface itself
static Tile *surface_height(int zoom, int x, int y) {
  int pass = V.pass;
  V.pass = 0;
  Tile *t = find_loaded(HEIGHT, zoom, x, y);
  V.pass = pass;
  return t;
}

static void draw_level_3d(int zoom, int radius, bool coarse, double ex, double ez, float textured, vector<BlockyPatch> &blocky) {
  const double size = tile_blocks(zoom), px = size / TILE_PX;
  const int tx = (int)floor(V.cx / size), tz = (int)floor(V.cz / size);
  // forward on the ground, to skip tiles behind the camera
  const double fx = -sin(V.yaw), fz = -cos(V.yaw);
  const bool steps_ok = zoom == MAX_ZOOM;
  const auto &p = V.smooth;

  for (int y = tz - radius; y <= tz + radius; y++) {
    for (int x = tx - radius; x <= tx + radius; x++) {
      double mx = (x + 0.5) * size, mz = (y + 0.5) * size;
      double ahead = (mx - ex) * fx + (mz - ez) * fz;
      if (ahead < -size) continue;
      if (off_screen(x * size - ex, y * size - ez, size)) continue;

      double dx = mx - V.cx, dz = mz - V.cz, prio = dx * dx + dz * dz;
      // distance from the target to the tile, in its pixels. caves are only drawn up close
      double nx = fmax(x * size - V.cx, fmax(0.0, V.cx - (x + 1) * size));
      double nz = fmax(y * size - V.cz, fmax(0.0, V.cz - (y + 1) * size));
      bool far = coarse || hypot(nx, nz) / px > full_r;
      if (V.pass && far) continue;

      want(COLOR, zoom, x, y, prio);
      want(HEIGHT, zoom, x, y, prio);
      Tile *c = find_loaded(COLOR, zoom, x, y);
      Tile *h = find_loaded(HEIGHT, zoom, x, y);
      Tile *surface = V.pass ? surface_height(zoom, x, y) : h;
      if (!c || !h || !surface) continue;

      Tile *icons = nullptr;
      if (textured > 0) {
        want(ICONS, zoom, x, y, prio);
        icons = find_loaded(ICONS, zoom, x, y);
      }
      glActiveTexture(GL_TEXTURE7);
      glBindTexture(GL_TEXTURE_2D, surface->tex);
      bind_tile(c, h, icons);
      glUniform1f(p.textured, icons ? textured : 0);

      if (far) {
        use_mesh(coarse ? V.grid64 : V.grid128);
        glUniform3f(p.tile, (float)(x * size - ex), (float)(y * size - ez), (float)size);
        glUniform3f(p.sub, 0, 0, 1);
        glUniform1f(p.quads, coarse ? 64 : 128);
        glDrawElements(GL_TRIANGLES, (coarse ? V.grid64 : V.grid128).count, GL_UNSIGNED_SHORT, 0);
        continue;
      }

      const double psize = size / PATCHES;
      for (int pz = 0; pz < PATCHES; pz++) {
        for (int pxi = 0; pxi < PATCHES; pxi++) {
          double x0 = x * size + pxi * psize, z0 = y * size + pz * psize;
          if (off_screen(x0 - ex, z0 - ez, psize)) continue;
          double d = hypot(x0 + psize / 2 - V.cx, z0 + psize / 2 - V.cz) / px;
          if (steps_ok && d < blocky_r) {
            // most regions have no shape tile at all, a missing one means only full blocks.
            // cave layers have none, shapes are a surface thing
            Tile *shapes = nullptr;
            if (!V.pass) {
              want(SHAPES, zoom, x, y, prio);
              shapes = find_loaded(SHAPES, zoom, x, y);
            }
            blocky.push_back({c, h, icons, shapes, surface, (float)(x0 - ex), (float)(z0 - ez), (float)(pxi * PATCH_PX), (float)(pz * PATCH_PX), icons ? textured : 0});
            continue;
          }
          const auto &m = d < full_r ? V.grid32 : V.grid8;
          use_mesh(m);
          glUniform3f(p.tile, (float)(x0 - ex), (float)(z0 - ez), (float)psize);
          glUniform3f(p.sub, (float)pxi / PATCHES, (float)pz / PATCHES, 1.0f / PATCHES);
          glUniform1f(p.quads, d < full_r ? 32 : 8);
          glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_SHORT, 0);
        }
      }
    }
  }
}

static void draw_blocky(const vector<BlockyPatch> &blocky, const Mat4 &mvp, float ey, double reach, float alpha) {
  if (blocky.empty()) return;
  const auto &p = V.steps;
  use_prog3d(p, mvp, ey, reach);
  layer_uniforms(p);
  glUniform1f(p.alpha, alpha);
  glUniform1i(p.shapes, 5);
  glUniform1i(p.profiles, 6);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D, V.profiles);
  glBindBuffer(GL_ARRAY_BUFFER, V.blocky.vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, V.blocky.ibo);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(2 * sizeof(float)));
  for (auto &b : blocky) {
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, b.shapes ? b.shapes->tex : V.no_shapes);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, b.surface->tex);
    bind_tile(b.c, b.h, b.icons);
    glUniform1f(p.textured, b.textured);
    glUniform3f(p.tile, b.x0, b.z0, 1);
    glUniform2f(p.patch, b.px, b.pz);
    glDrawElements(GL_TRIANGLES, V.blocky.count, GL_UNSIGNED_SHORT, 0);
  }
  glDisableVertexAttribArray(1);
}

// one scene: the surface, and with x-ray the caves of every layer underneath it
static void draw_3d() {
  double ex, ey, ez;
  eye_pos(ex, ey, ez);
  // always full resolution near the camera, however far out it is
  const int zoom = MAX_ZOOM;
  const double size = tile_blocks(zoom);
  const int radius = min(3, max(1, (int)ceil(V.dist * 2.5 / size)));
  blocky_r = fmin(1024.0, fmax(160.0, V.dist * 3));
  full_r = blocky_r * 2;
  // plus a ring of coarse tiles out to the horizon, sized to how far out the camera is
  const int coarse = max(MIN_ZOOM, zoom_for(blocks_per_px_3d()) - 3);
  const double reach = coarse < zoom ? 3.5 * tile_blocks(coarse) : (radius + 0.5) * size;

  Mat4 proj = perspective(FOV, (double)V.width / V.height, fmax(0.5, V.dist * 0.005), reach * 2);
  Mat4 mvp = mul(proj, look(V.cx - ex, V.ty - ey, V.cz - ez));
  cull_mvp = mvp;
  cull_ey = ey;

  glEnable(GL_DEPTH_TEST);
  if (V.textures) bind_texture_units(3);
  V.pass = 0;
  use_prog3d(V.smooth, mvp, (float)ey, reach);
  layer_uniforms(V.smooth);

  vector<BlockyPatch> blocky;
  if (coarse < zoom) {
    draw_level_3d(coarse, 3, true, ex, ez, 0, blocky);
    // the detailed tiles always win, they sit inside the coarse ones
    glClear(GL_DEPTH_BUFFER_BIT);
  }
  // block textures on the detailed tiles once they're close enough to read
  float textured = zoom == MAX_ZOOM ? texture_fade(1.0 / blocks_per_px_3d()) : 0;

  // caves first, so the see-through surface blends over them
  if (V.xray < 1) {
    for (V.pass = 1; V.pass < LAYERS; V.pass++) {
      vector<BlockyPatch> cave_blocky;
      use_prog3d(V.smooth, mvp, (float)ey, reach);
      layer_uniforms(V.smooth);
      draw_level_3d(zoom, radius, false, ex, ez, textured, cave_blocky);
      draw_blocky(cave_blocky, mvp, (float)ey, reach, 1);
    }
    V.pass = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  // a see-through surface must not hide the caves behind it in the depth buffer
  if (V.xray < 1) glDepthMask(GL_FALSE);
  use_prog3d(V.smooth, mvp, (float)ey, reach);
  layer_uniforms(V.smooth);
  glUniform1f(V.smooth.alpha, V.xray);
  draw_level_3d(zoom, radius, false, ex, ez, textured, blocky);
  draw_blocky(blocky, mvp, (float)ey, reach, V.xray);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);
}

static void frame() {
  // while tiles are loading keep drawing, every frame queues the next requests
  if (!V.dirty && V.inflight == 0) return;
  bool was_dirty = V.dirty;
  V.dirty = false;
  V.frame++;

  glViewport(0, 0, V.width, V.height);
  glClearColor(BG[0], BG[1], BG[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  V.wanted.clear();
  if (V.mode3d) draw_3d();
  else draw_2d();

  sort(V.wanted.begin(), V.wanted.end(), [](auto &a, auto &b) { return a.priority < b.priority; });
  for (auto &w : V.wanted) {
    if (V.inflight >= MAX_INFLIGHT) break;
    request_tile(w);
  }
  if (was_dirty) evict();
}

// setup

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    printf("shader: %s\n", log);
  }
  return s;
}

static GLuint link(const char *vs, const char *fs, const char *attrib) {
  GLuint p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
  glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(p, 0, attrib);
  glLinkProgram(p);
  return p;
}

// picks the block texture for a point of a zoom 17 tile, shared by the 2d and 3d shaders
// p is in tile pixels, one pixel per block. returns the textured color, or the alpha 0 when no icon
static const char *TEXTURE_GLSL = R"(
    uniform sampler2D u_icons;
    uniform sampler2D u_atlas;
    uniform sampler2D u_tint;
    uniform vec2 u_atlas_size;
    uniform vec2 u_tint_size;
    uniform float u_cols;
    vec4 block_texture(highp vec2 p) {
      highp vec2 block = floor(clamp(p, 0.0, 511.0));
      vec4 t = texture2D(u_icons, (block + 0.5) / 512.0);
      highp float index = floor(t.r * 255.0 + 0.5) + floor(t.g * 255.0 + 0.5) * 256.0;
      if (index < 0.5) return vec4(0.0);
      index -= 1.0;
      highp vec2 cell = vec2(mod(index, u_cols), floor(index / u_cols));
      highp vec2 texel = floor(fract(p) * 16.0);
      vec4 c = texture2D(u_atlas, (cell * 16.0 + texel + 0.5) / u_atlas_size);
      vec3 tint = texture2D(u_tint, (vec2(mod(index, 256.0), floor(index / 256.0)) + 0.5) / u_tint_size).rgb;
      return vec4(c.rgb * tint, c.a);
    }
)";

static void setup_gl() {
  // glsl 1.00 so the same shaders run on webgl 1 and 2
  V.prog2d = link(R"(
    attribute vec2 a_corner;
    uniform vec4 u_rect; // x0, y0, x1, y1 in ndc
    varying vec2 v_uv;
    void main() {
      v_uv = a_corner;
      gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_corner), 0.0, 1.0);
    })", R"(
    precision mediump float;
    uniform sampler2D u_tex;
    varying vec2 v_uv;
    void main() {
      vec4 c = texture2D(u_tex, v_uv);
      // unexplored chunks are fully transparent, let the background through
      gl_FragColor = vec4(mix(vec3(0.0), c.rgb, c.a), 1.0);
    })", "a_corner");
  V.u_rect = glGetUniformLocation(V.prog2d, "u_rect");
  V.u_tex = glGetUniformLocation(V.prog2d, "u_tex");

  string tex_fs = string(R"(
    precision highp float;
    uniform sampler2D u_color;
    uniform float u_fade;
    varying vec2 v_uv;
    )") + TEXTURE_GLSL + R"(
    void main() {
      vec4 c = texture2D(u_color, v_uv);
      vec3 flat_color = mix(vec3(0.0), c.rgb, c.a);
      vec4 t = block_texture(v_uv * 512.0);
      // see-through texels (flowers, glass, leaves) show the flat color behind them
      gl_FragColor = vec4(mix(flat_color, mix(flat_color, t.rgb, t.a), u_fade), 1.0);
    })";
  V.prog_tex = link(R"(
    attribute vec2 a_corner;
    uniform vec4 u_rect;
    varying vec2 v_uv;
    void main() {
      v_uv = a_corner;
      gl_Position = vec4(mix(u_rect.xy, u_rect.zw, a_corner), 0.0, 1.0);
    })", tex_fs.c_str(), "a_corner");
  V.ut_rect = glGetUniformLocation(V.prog_tex, "u_rect");
  V.ut_color = glGetUniformLocation(V.prog_tex, "u_color");
  V.ut_icons = glGetUniformLocation(V.prog_tex, "u_icons");
  V.ut_atlas = glGetUniformLocation(V.prog_tex, "u_atlas");
  V.ut_tint = glGetUniformLocation(V.prog_tex, "u_tint");
  V.ut_atlas_size = glGetUniformLocation(V.prog_tex, "u_atlas_size");
  V.ut_tint_size = glGetUniformLocation(V.prog_tex, "u_tint_size");
  V.ut_cols = glGetUniformLocation(V.prog_tex, "u_cols");
  V.ut_fade = glGetUniformLocation(V.prog_tex, "u_fade");

  for (GLuint *t : { &V.atlas, &V.tint }) {
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  js_load_textures(V.atlas, V.tint);

  const float corners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
  glGenBuffers(1, &V.quad);
  glBindBuffer(GL_ARRAY_BUFFER, V.quad);
  glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);

  // 3d needs textures in the vertex shader, optional in webgl 1 but nearly everywhere
  GLint vtex = 0;
  glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vtex);
  // the block steps read heights, shapes and shape profiles in the vertex shader
  V.can3d = vtex >= 4;
  if (!V.can3d) return;

  // shared by both 3d programs: fog, lighting and the textures on top of the flat color
  string fs_common = string(R"(
    precision highp float;
    uniform sampler2D u_color;
    uniform vec2 u_fog;       // start, end in blocks from the eye
    uniform float u_textured; // 0..1, block textures fading in up close
    uniform float u_cave;     // 1 in a cave layer, where empty columns are solid rock rather than void
    uniform float u_alpha;    // the surface goes see-through for x-ray
    varying float v_light;
    varying float v_dist;
    varying float v_keep;     // below 0.5 this belongs to another layer
    )") + TEXTURE_GLSL + R"(
    vec4 shade(vec2 color_uv, vec2 p) {
      vec3 bg = vec3(0.0);
      vec4 c = texture2D(u_color, color_uv);
      // in a cave layer a transparent column is solid rock, it only shows as the walls of caves
      if (u_cave > 0.5) c = vec4(mix(vec3(0.2, 0.2, 0.22), c.rgb, c.a), 1.0);
      vec3 base = c.rgb;
      if (u_textured > 0.0) {
        vec4 t = block_texture(p);
        base = mix(base, mix(base, t.rgb, t.a), u_textured);
      }
      vec3 lit = mix(bg, base * v_light, c.a);
      float fog = clamp((v_dist - u_fog.x) / (u_fog.y - u_fog.x), 0.0, 1.0);
      return vec4(mix(lit, bg, fog), u_alpha);
    }
  )";
  // the height of a tile pixel. the zoom levels average heights together with alpha, dividing undoes that
  const char *height_glsl = R"(
    uniform sampler2D u_heightmap;
    uniform sampler2D u_surface;  // the surface's heights, under a cave layer
    uniform mat4 u_mvp;
    uniform float u_eye;      // eye height
    uniform float u_cut;      // cave layer: everything above this was cut away, 0 for the surface
    uniform float u_band;     // cave layer: floors below this belong to the next layer down
    varying float v_light;
    varying float v_dist;
    varying float v_keep;
    float height_px(vec2 px) {
      vec4 t = texture2DLod(u_heightmap, (clamp(floor(px), 0.0, 511.0) + 0.5) / 512.0, 0.0);
      return t.a > 0.01 ? t.r / t.a * 255.0 : 0.0;
    }
    // whether a cave layer draws this pixel's floor: not rock (which sits at the cut), not another
    // layer's floor, not the surface seen through open air
    float keep_px(vec2 px) {
      if (u_cut < 0.5) return 1.0;
      float h = height_px(px);
      vec4 s = texture2DLod(u_surface, (clamp(floor(px), 0.0, 511.0) + 0.5) / 512.0, 0.0);
      float surface = s.a > 0.01 ? s.r / s.a * 255.0 : 0.0;
      if (h > u_cut - 0.5 || h < u_band - 0.5 || abs(h - surface) < 0.5) return 0.0;
      return 1.0;
    }
    void light(vec3 n) {
      v_light = 0.5 + 0.6 * max(dot(n, normalize(vec3(-0.4, 1.0, -0.6))), 0.0);
    }
  )";

  // smooth surface: vertex k of an edge sits on pixel k, so it meets the block steps exactly
  string vs_smooth = string(R"(
    attribute vec3 a_uv;      // x, z in 0..1 over the patch, and 1 for the skirt hanging off its edges
    uniform vec3 u_tile;      // x, z of the patch corner relative to the eye, its size in blocks
    uniform vec3 u_sub;       // the patch inside its tile: uv offset and size
    uniform float u_quads;    // quads per patch side
    varying vec2 v_uv;
    )") + height_glsl + R"(
    void main() {
      vec2 uv = u_sub.xy + a_uv.xy * u_sub.z;
      v_uv = uv;
      vec2 px = uv * 512.0 + 0.001;
      float step = u_tile.z / u_quads;
      float dpx = 512.0 * u_sub.z / u_quads;
      float h = height_px(px);
      v_keep = keep_px(px);
      // skirts drop below the surface to hide the cracks where detail levels meet
      float y = h - u_eye - a_uv.z * max(8.0, step * 2.0);
      vec3 pos = vec3(u_tile.x + a_uv.x * u_tile.z, y, u_tile.y + a_uv.y * u_tile.z);
      light(normalize(vec3(height_px(px - vec2(dpx, 0.0)) - height_px(px + vec2(dpx, 0.0)), 2.0 * step,
                           height_px(px - vec2(0.0, dpx)) - height_px(px + vec2(0.0, dpx)))));
      v_dist = length(pos.xz);
      gl_Position = u_mvp * vec4(pos, 1.0);
    })";
  string fs_smooth = fs_common + R"(
    varying vec2 v_uv;
    void main() {
      if (v_keep < 0.5) discard;
      gl_FragColor = shade(v_uv, v_uv * 512.0);
    })";

  // block steps: a top per block, and walls on its east and south side down to the neighbor.
  // tops are a 3x3 grid so ArchitectureCraft slopes, ridges and slabs can bend them
  string vs_steps = string(R"(
    attribute vec2 a_pos;     // corner in blocks from the patch corner
    attribute vec4 a_info;    // block in the patch, face (0 top, 1 east wall, 2 south wall), which block's side
    uniform vec3 u_tile;      // x, z of the patch corner relative to the eye
    uniform vec2 u_patch;     // the patch's first pixel in its tile
    uniform sampler2D u_shapes;
    uniform sampler2D u_profiles;
    varying vec2 v_block;     // the block whose texture this face shows
    varying vec2 v_face_uv;   // where on that block's texture
    )") + height_glsl + R"(
    // surface height of a block at grid point g (0..2 on x and z, 1 is the middle)
    float surface(vec2 block, vec2 g) {
      float h = height_px(block);
      vec4 s = texture2DLod(u_shapes, (clamp(block, 0.0, 511.0) + 0.5) / 512.0, 0.0);
      float id = floor(s.r * 255.0 + 0.5);
      if (id < 0.5) return h;
      // into the shape's own orientation: a turn takes south to east, so undo it a quarter at a time
      float turn = floor(s.g * 255.0 + 0.5);
      for (int k = 0; k < 3; k++)
        if (float(k) < turn) g = vec2(2.0 - g.y, g.x);
      float v = texture2DLod(u_profiles, (vec2(g.x + g.y * 3.0, id - 1.0) + 0.5) / vec2(9.0, 256.0), 0.0).r;
      // the top face sits at h, so a full block spans h - 1 .. h
      return h - 1.0 + v;
    }
    void main() {
      vec2 a = u_patch + a_info.xy;
      float face = a_info.z;
      vec2 corner = a_pos - a_info.xy;      // 0..1 inside block a
      vec2 b = a;
      vec2 ga = corner * 2.0, gb = ga;
      if (face == 1.0) { b = a + vec2(1.0, 0.0); ga = vec2(2.0, corner.y * 2.0); gb = vec2(0.0, ga.y); }
      if (face == 2.0) { b = a + vec2(0.0, 1.0); ga = vec2(corner.x * 2.0, 2.0); gb = vec2(ga.x, 0.0); }
      float ha = surface(a, ga);
      float hb = surface(b, gb);
      float ka = keep_px(a), kb = keep_px(b);
      // a wall belongs to the cave on either side of it. in a cave layer its other end stays inside
      // the layer's band, deeper floors are another layer's to wall in
      if (u_cut > 0.5) {
        if (ka < 0.5) ha = clamp(ha, u_band, u_cut);
        if (kb < 0.5) hb = clamp(hb, u_band, u_cut);
      }
      float y = face == 0.0 ? ha : mix(ha, hb, a_info.w);
      v_keep = face == 0.0 ? ka : max(ka, kb);
      vec3 pos = vec3(u_tile.x + a_pos.x, y - u_eye, u_tile.y + a_pos.y);
      if (face == 0.0) {
        // slope of the top from its neighboring grid points
        vec2 gx0 = vec2(max(ga.x - 1.0, 0.0), ga.y), gx1 = vec2(min(ga.x + 1.0, 2.0), ga.y);
        vec2 gz0 = vec2(ga.x, max(ga.y - 1.0, 0.0)), gz1 = vec2(ga.x, min(ga.y + 1.0, 2.0));
        float dx = (surface(a, gx1) - surface(a, gx0)) / ((gx1.x - gx0.x) * 0.5);
        float dz = (surface(a, gz1) - surface(a, gz0)) / ((gz1.y - gz0.y) * 0.5);
        light(normalize(vec3(-dx, 1.0, -dz)));
        v_block = a;
        v_face_uv = corner;
      } else {
        float side = ha >= hb ? 1.0 : -1.0;
        light(face == 1.0 ? vec3(side, 0.0, 0.0) : vec3(0.0, 0.0, side));
        v_block = ha >= hb ? a : b;
        // across the wall horizontally, down it by height
        v_face_uv = vec2(face == 1.0 ? corner.y : corner.x, -y);
      }
      v_dist = length(pos.xz);
      gl_Position = u_mvp * vec4(pos, 1.0);
    })";
  string fs_steps = fs_common + R"(
    varying vec2 v_block;
    varying vec2 v_face_uv;
    void main() {
      if (v_keep < 0.5) discard;
      vec2 f = clamp(fract(v_face_uv), 0.0, 0.999);
      gl_FragColor = shade((v_block + 0.5) / 512.0, v_block + f);
    })";

  auto setup = [](decltype(V.smooth) &p, const string &vs, const string &fs, const char *attrib) {
    p.id = link(vs.c_str(), fs.c_str(), attrib);
    if (attrib == string("a_pos")) {
      glBindAttribLocation(p.id, 1, "a_info");
      glLinkProgram(p.id);
    }
    for (auto [loc, name] : initializer_list<pair<GLint *, const char *>>{
           { &p.mvp, "u_mvp" }, { &p.tile, "u_tile" }, { &p.sub, "u_sub" }, { &p.quads, "u_quads" },
           { &p.eye, "u_eye" }, { &p.fog, "u_fog" }, { &p.color, "u_color" }, { &p.heightmap, "u_heightmap" },
           { &p.icons, "u_icons" }, { &p.atlas, "u_atlas" }, { &p.tint, "u_tint" }, { &p.atlas_size, "u_atlas_size" },
           { &p.tint_size, "u_tint_size" }, { &p.cols, "u_cols" }, { &p.textured, "u_textured" }, { &p.patch, "u_patch" },
           { &p.shapes, "u_shapes" }, { &p.profiles, "u_profiles" }, { &p.cave, "u_cave" },
           { &p.cut, "u_cut" }, { &p.band, "u_band" }, { &p.surface, "u_surface" }, { &p.alpha, "u_alpha" } })
      *loc = glGetUniformLocation(p.id, name);
  };
  setup(V.smooth, vs_smooth, fs_smooth, "a_uv");
  setup(V.steps, vs_steps, fs_steps, "a_pos");

  // n x n quads plus a skirt: the edge ring again with z = 1, joined to the edge by a strip of quads
  auto grid = [](int n) {
    decltype(V.grid8) m;
    vector<float> v;
    for (int y = 0; y <= n; y++)
      for (int x = 0; x <= n; x++) v.insert(v.end(), { (float)x / n, (float)y / n, 0 });
    vector<uint16_t> idx;
    auto at = [n](int x, int y) { return (uint16_t)(y * (n + 1) + x); };
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++) {
        uint16_t a = at(x, y), b = at(x + 1, y), c = at(x, y + 1), d = at(x + 1, y + 1);
        idx.insert(idx.end(), { a, c, b, b, c, d });
      }
    // walk the border, every border vertex gets a dropped twin
    vector<uint16_t> ring;
    for (int x = 0; x < n; x++) ring.push_back(at(x, 0));
    for (int y = 0; y < n; y++) ring.push_back(at(n, y));
    for (int x = n; x > 0; x--) ring.push_back(at(x, n));
    for (int y = n; y > 0; y--) ring.push_back(at(0, y));
    uint16_t base = v.size() / 3;
    for (uint16_t r : ring) v.insert(v.end(), { v[r * 3], v[r * 3 + 1], 1 });
    for (size_t i = 0; i < ring.size(); i++) {
      uint16_t a = ring[i], b = ring[(i + 1) % ring.size()];
      uint16_t c = base + i, d = base + (i + 1) % ring.size();
      // both windings, the skirt is seen from either side
      idx.insert(idx.end(), { a, b, c, b, d, c, a, c, b, b, c, d });
    }
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint16_t), idx.data(), GL_STATIC_DRAW);
    m.count = idx.size();
    return m;
  };
  // top surface of each ArchitectureCraft shape at turn 0, 3x3 heights in blocks (row = z, north to south).
  // turn 0 slopes rise towards the south, see the shapes tile in biome-thingy. unknown shapes stay full
  {
    vector<uint8_t> tex(9 * 256 * 4, 255);
    auto set = [&](int id, auto f) {
      for (int gz = 0; gz < 3; gz++)
        for (int gx = 0; gx < 3; gx++) tex[(id * 9 + gz * 3 + gx) * 4] = (uint8_t)(fmin(1.0, fmax(0.0, f(gx * 0.5, gz * 0.5))) * 255);
    };
    auto slope = [](double lo, double hi) { return [=](double, double z) { return lo + (hi - lo) * z; }; };
    auto tile = slope(0, 1);
    auto outer = [](double x, double z) { return fmin(x, z); };
    auto inner = [](double x, double z) { return fmax(x, z); };
    auto ridge = [](double, double z) { return 1 - fabs(z - 0.5); };
    auto valley = [](double, double z) { return 0.5 + fabs(z - 0.5); };
    auto slab = [](double, double) { return 0.5; };
    auto stairs = slope(0.5, 1);
    for (int id : { 0, 7, 40, 41, 42, 43, 112, 116 }) set(id, tile);           // roof tiles and overhangs
    for (int id : { 1, 8 }) set(id, outer);                                      // outer corners
    for (int id : { 2, 9 }) set(id, inner);                                      // inner corners
    for (int id : { 3, 4, 44, 115 }) set(id, ridge);
    for (int id : { 5, 6, 45 }) set(id, valley);
    for (int id : { 90, 114 }) set(id, slab);
    set(91, stairs);
    set(92, [](double x, double z) { return 0.5 + 0.5 * fmin(x, z); });
    set(93, [](double x, double z) { return 0.5 + 0.5 * fmax(x, z); });
    // slope tiles spread one rise over 2, 3 or 4 blocks, A1 being the lowest part (and the SE variants alike)
    const double parts[][2] = { { 0, .5 }, { .5, 1 }, { 0, 1 / 3. }, { 1 / 3., 2 / 3. }, { 2 / 3., 1 },
                                { 0, .25 }, { .25, .5 }, { .5, .75 }, { .75, 1 } };
    for (int i = 0; i < 9; i++) {
      set(94 + i, slope(parts[i][0], parts[i][1]));
      set(103 + i, slope(parts[i][0], parts[i][1]));
    }
    glGenTextures(1, &V.profiles);
    glBindTexture(GL_TEXTURE_2D, V.profiles);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 9, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.data());
    const uint8_t zero[4] = { 0, 0, 0, 0 };
    glGenTextures(1, &V.no_shapes);
    glBindTexture(GL_TEXTURE_2D, V.no_shapes);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    for (GLuint t : { V.profiles, V.no_shapes }) {
      glBindTexture(GL_TEXTURE_2D, t);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
  }

  V.grid128 = grid(128);
  V.grid64 = grid(64);
  V.grid32 = grid(32);
  V.grid8 = grid(8);

  // 32x32 blocks: 9 vertices per top, 4 per east and south wall. 17408 vertices, fits 16 bit indices
  {
    vector<float> v;
    vector<uint16_t> idx;
    for (int bz = 0; bz < PATCH_PX; bz++) {
      for (int bx = 0; bx < PATCH_PX; bx++) {
        float x = bx, z = bz;
        auto put = [&](float px, float pz, float face, float sel) { v.insert(v.end(), { px, pz, x, z, face, sel }); };
        uint16_t i = v.size() / 6;
        for (int gz = 0; gz < 3; gz++)
          for (int gx = 0; gx < 3; gx++) put(x + gx * 0.5f, z + gz * 0.5f, 0, 0);
        for (int gz = 0; gz < 2; gz++)
          for (int gx = 0; gx < 2; gx++) {
            uint16_t a = i + gz * 3 + gx, b = a + 1, c = a + 3, d = a + 4;
            idx.insert(idx.end(), { a, c, b, b, c, d });
          }
        i += 9;
        // east wall at x + 1 between this block's surface and the east neighbor's, south wall at z + 1
        put(x + 1, z, 1, 0); put(x + 1, z + 1, 1, 0); put(x + 1, z, 1, 1); put(x + 1, z + 1, 1, 1);
        put(x, z + 1, 2, 0); put(x + 1, z + 1, 2, 0); put(x, z + 1, 2, 1); put(x + 1, z + 1, 2, 1);
        for (int q = 0; q < 2; q++) {
          uint16_t a = i + q * 4, b = a + 1, c = a + 2, d = a + 3;
          // both windings, a wall faces either way depending on which side is higher
          idx.insert(idx.end(), { a, c, b, b, c, d, a, b, c, b, d, c });
        }
      }
    }
    glGenBuffers(1, &V.blocky.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, V.blocky.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &V.blocky.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, V.blocky.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint16_t), idx.data(), GL_STATIC_DRAW);
    V.blocky.count = idx.size();
  }
}

// input

static void zoom_at(double px, double py, double factor) {
  if (V.mode3d) {
    V.dist = fmin(200000.0, fmax(4.0, V.dist / factor));
    V.dirty = true;
    return;
  }
  // keep the block under the cursor in place
  double bx = V.cx + (px - V.width / 2.0) / V.scale, bz = V.cz + (py - V.height / 2.0) / V.scale;
  V.scale = fmin(16.0, fmax(1.0 / (1 << (MAX_ZOOM - MIN_ZOOM + 2)), V.scale * factor));
  V.cx = bx - (px - V.width / 2.0) / V.scale;
  V.cz = bz - (py - V.height / 2.0) / V.scale;
  V.dirty = true;
}

static void pan(double dx, double dy) {
  if (V.mode3d) {
    // drag the ground: right is along the screen's x, down pulls the ground towards the camera
    double k = blocks_per_px_3d() / fmax(0.3, sin(V.pitch));
    double rx = cos(V.yaw), rz = -sin(V.yaw), fx = -sin(V.yaw), fz = -cos(V.yaw);
    V.cx -= rx * dx * blocks_per_px_3d() - fx * dy * k;
    V.cz -= rz * dx * blocks_per_px_3d() - fz * dy * k;
    js_show_cursor(floor(V.cx), floor(V.cz));
  } else {
    V.cx -= dx / V.scale;
    V.cz -= dy / V.scale;
  }
  V.dirty = true;
}

static void rotate(double dx, double dy) {
  V.yaw -= dx * 0.005;
  V.pitch = fmin(1.5, fmax(0.15, V.pitch + dy * 0.005));
  V.dirty = true;
}

static void show_cursor(double px, double py) {
  if (V.mode3d) return;  // 3d shows the orbit target instead
  js_show_cursor(floor(V.cx + (px - V.width / 2.0) / V.scale), floor(V.cz + (py - V.height / 2.0) / V.scale));
}

// events come in css pixels, the canvas is sized in device pixels
static double dpr() { return emscripten_get_device_pixel_ratio(); }

static bool on_mouse(int type, const EmscriptenMouseEvent *e, void *) {
  double x = e->targetX * dpr(), y = e->targetY * dpr();
  if (type == EMSCRIPTEN_EVENT_MOUSEDOWN) {
    // right drag or shift drag orbits in 3d
    if (e->button == 2 || (e->button == 0 && e->shiftKey)) V.rotating = true;
    else if (e->button == 0) V.dragging = true;
  } else if (type == EMSCRIPTEN_EVENT_MOUSEUP) {
    V.dragging = V.rotating = false;
  } else if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) {
    if (V.rotating && V.mode3d) rotate(x - V.drag_x, y - V.drag_y);
    else if (V.dragging || V.rotating) pan(x - V.drag_x, y - V.drag_y);
    show_cursor(x, y);
  }
  V.drag_x = x;
  V.drag_y = y;
  return true;
}

static bool on_wheel(int, const EmscriptenWheelEvent *e, void *) {
  double dy = e->deltaY;
  if (e->deltaMode == DOM_DELTA_LINE) dy *= 40;
  zoom_at(e->mouse.targetX * dpr(), e->mouse.targetY * dpr(), pow(1.0015, -dy));
  return true;
}

static bool on_touch(int type, const EmscriptenTouchEvent *e, void *) {
  if (e->numTouches == 1) {
    double x = e->touches[0].targetX * dpr(), y = e->touches[0].targetY * dpr();
    if (type == EMSCRIPTEN_EVENT_TOUCHMOVE && V.pinch_dist == 0) pan(x - V.drag_x, y - V.drag_y);
    V.drag_x = x;
    V.drag_y = y;
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) V.pinch_dist = 0;
  } else if (e->numTouches >= 2) {
    double ax = e->touches[0].targetX * dpr(), ay = e->touches[0].targetY * dpr();
    double bx = e->touches[1].targetX * dpr(), by = e->touches[1].targetY * dpr();
    double d = hypot(ax - bx, ay - by), angle = atan2(by - ay, bx - ax), my = (ay + by) / 2;
    if (type == EMSCRIPTEN_EVENT_TOUCHMOVE && V.pinch_dist > 0) {
      zoom_at((ax + bx) / 2, my, d / V.pinch_dist);
      // in 3d, twisting turns the camera and moving both fingers up or down tilts it
      if (V.mode3d) rotate((angle - V.pinch_angle) * -200, my - V.pinch_y);
    }
    V.pinch_dist = d;
    V.pinch_angle = angle;
    V.pinch_y = my;
  }
  if (type == EMSCRIPTEN_EVENT_TOUCHEND || type == EMSCRIPTEN_EVENT_TOUCHCANCEL) {
    if (e->numTouches <= 1) V.pinch_dist = 0;
    // the finger that stays down becomes the new drag origin
    for (int i = 0; i < e->numTouches; i++) {
      if (!e->touches[i].isChanged) { V.drag_x = e->touches[i].targetX * dpr(); V.drag_y = e->touches[i].targetY * dpr(); }
    }
  }
  return true;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void viewer_resize(int w, int h) {
  V.width = w;
  V.height = h;
  V.dirty = true;
}

EMSCRIPTEN_KEEPALIVE void viewer_set_snapshot(int sid) {
  if (sid == V.sid) return;
  V.prev_sid = V.sid;
  V.sid = sid;
  V.dirty = true;
}

EMSCRIPTEN_KEEPALIVE void viewer_set_dim(const char *dim) {
  V.dim = dim;
  clear_tiles();
  V.prev_sid = V.sid;
  V.dirty = true;
}

// 2d: 0 for the surface, or a cave layer's y
EMSCRIPTEN_KEEPALIVE void viewer_set_layer(int y) {
  V.layer = 0;
  for (int i = 1; i < LAYERS; i++)
    if (CAVE_LEVELS[i] == y) V.layer = i;
  V.dirty = true;
}

// 3d: surface opacity, 1 hides the caves, lower shows them through it
EMSCRIPTEN_KEEPALIVE void viewer_set_xray(double opacity) {
  V.xray = (float)fmin(1.0, fmax(0.0, opacity));
  V.dirty = true;
}

EMSCRIPTEN_KEEPALIVE void viewer_goto(double x, double z) {
  V.cx = x;
  V.cz = z;
  V.dirty = true;
}

EMSCRIPTEN_KEEPALIVE int viewer_can_3d() {
  return V.can3d;
}

EMSCRIPTEN_KEEPALIVE void viewer_set_scale(double scale) {
  V.scale = fmin(16.0, fmax(1.0 / 4096, scale));
  V.dirty = true;
}

// 3d orbit camera, angles in degrees, for links to a view
EMSCRIPTEN_KEEPALIVE void viewer_set_camera(double yaw, double pitch, double dist) {
  V.yaw = yaw * M_PI / 180;
  V.pitch = fmin(1.5, fmax(0.15, pitch * M_PI / 180));
  V.dist = fmin(200000.0, fmax(4.0, dist));
  V.dirty = true;
}

// switches between 2d and 3d, keeping roughly the same area on screen
EMSCRIPTEN_KEEPALIVE void viewer_set_3d(int on) {
  if (!V.can3d || (bool)on == V.mode3d) return;
  if (on) V.dist = V.height / V.scale / (2 * tan(FOV / 2));
  else V.scale = V.height / (V.dist * 2 * tan(FOV / 2));
  V.mode3d = on;
  V.dirty = true;
}
}

int main() {
  EmscriptenWebGLContextAttributes attr;
  emscripten_webgl_init_context_attributes(&attr);
  attr.majorVersion = 2;
  attr.alpha = false;
  attr.antialias = true;
  attr.depth = true;
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#map", &attr);
  if (ctx <= 0) {
    // webgl 2 is blocked on some drivers, nothing here needs more than webgl 1
    attr.majorVersion = 1;
    ctx = emscripten_webgl_create_context("#map", &attr);
  }
  if (ctx <= 0) {
    printf("webgl is not available (error %d)\n", (int)ctx);
    js_fail((int)ctx);
    return 1;
  }
  emscripten_webgl_make_context_current(ctx);
  setup_gl();

  emscripten_set_mousedown_callback("#map", nullptr, true, on_mouse);
  emscripten_set_mousemove_callback("#map", nullptr, true, on_mouse);
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_mouse);
  emscripten_set_wheel_callback("#map", nullptr, true, on_wheel);
  emscripten_set_touchstart_callback("#map", nullptr, true, on_touch);
  emscripten_set_touchmove_callback("#map", nullptr, true, on_touch);
  emscripten_set_touchend_callback("#map", nullptr, true, on_touch);
  emscripten_set_touchcancel_callback("#map", nullptr, true, on_touch);

  emscripten_set_main_loop(frame, 0, false);
  js_ready();
  return 0;
}

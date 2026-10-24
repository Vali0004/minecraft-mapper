// SPDX-License-Identifier: MPL-2.0
#include "nbt_tree.h"

#include <cstring>
#include <stdexcept>
#include <zlib.h>

#include "utils.h"

using namespace std;

const NbtTag *NbtTag::get(const string &key) const {
  auto it = compound.find(key);
  return it == compound.end() ? nullptr : &it->second;
}

double NbtTag::num(const string &key, double def) const {
  const NbtTag *t = get(key);
  return t ? t->asNum() : def;
}

namespace {
struct Reader {
  const uint8_t *p;
  size_t size, pos = 0;

  void need(size_t n) {
    if (pos + n > size) throw runtime_error("nbt truncated");
  }
  uint64_t be(int bytes) {
    need(bytes);
    uint64_t v = 0;
    for (int k = 0; k < bytes; k++) v = (v << 8) | p[pos++];
    return v;
  }
  string str() {
    size_t len = be(2);
    need(len);
    string s((const char*)p + pos, len);
    pos += len;
    return s;
  }
  int32_t count() {
    int32_t n = (int32_t)be(4);
    if (n < 0) throw runtime_error("nbt negative length");
    return n;
  }

  void payload(NbtTag &t, uint8_t type, int depth) {
    if (depth > 512) throw runtime_error("nbt nested too deep");
    t.type = type;
    switch (type) {
    case 1: t.i = (int8_t)be(1); break;
    case 2: t.i = (int16_t)be(2); break;
    case 3: t.i = (int32_t)be(4); break;
    case 4: t.i = (int64_t)be(8); break;
    case 5: { uint32_t r = be(4); float f; memcpy(&f, &r, 4); t.d = f; break; }
    case 6: { uint64_t r = be(8); memcpy(&t.d, &r, 8); break; }
    case 7: case 11: case 12: {
      int width = type == 7 ? 1 : type == 11 ? 4 : 8;
      int32_t n = count();
      need((size_t)n * width);
      t.list.resize(n);
      for (auto &e : t.list) {
        e.type = width == 1 ? 1 : width == 4 ? 3 : 4;
        uint64_t v = be(width);
        e.i = width == 1 ? (int8_t)v : width == 4 ? (int32_t)v : (int64_t)v;
      }
      break;
    }
    case 8: t.s = str(); break;
    case 9: {
      uint8_t et = be(1);
      int32_t n = count();
      t.list.resize(n);
      for (auto &e : t.list) payload(e, et, depth + 1);
      break;
    }
    case 10:
      while (true) {
        uint8_t ct = be(1);
        if (ct == 0) break;
        string name = str();
        payload(t.compound[name], ct, depth + 1);
      }
      break;
    default:
      throw runtime_error("nbt unknown tag " + to_string(type));
    }
  }
};
}

NbtTag parseNbtTree(const string &data) {
  Reader r{(const uint8_t*)data.data(), data.size()};
  NbtTag root;
  uint8_t type = r.be(1);
  r.str();
  r.payload(root, type, 0);
  return root;
}

NbtTag loadGzipNbt(const filesystem::path &p) {
  const string in = readFile(p);
  z_stream strm{};
  if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) throw runtime_error("inflateInit failed");
  strm.next_in = (Bytef*)in.data();
  strm.avail_in = in.size();
  string out;
  char buf[65536];
  int ret;
  do {
    strm.next_out = (Bytef*)buf;
    strm.avail_out = sizeof(buf);
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&strm);
      throw runtime_error("gzip error in " + p.string());
    }
    out.append(buf, sizeof(buf) - strm.avail_out);
  } while (ret != Z_STREAM_END && strm.avail_in > 0);
  inflateEnd(&strm);
  return parseNbtTree(out);
}

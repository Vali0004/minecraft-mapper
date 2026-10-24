// SPDX-License-Identifier: MPL-2.0
#include "nbt.h"

using namespace std;

uint16_t getint16(const unsigned char *nbt, int *pos) {
  unsigned char a, b;
  a = nbt[(*pos)++];
  b = nbt[(*pos)++];
  return (a<<8) | b;
}

uint32_t getint32(const unsigned char *nbt, int *pos) {
  unsigned char a, b, c, d;
  a = nbt[(*pos)++];
  b = nbt[(*pos)++];
  c = nbt[(*pos)++];
  d = nbt[(*pos)++];
  return (a<<24) | (b << 16) | (c << 8) | d;
}

uint64_t getint64(const unsigned char *nbt, int *pos) {
  uint64_t a, b, c, d, e, f, g, i;
  a = nbt[(*pos)++];
  b = nbt[(*pos)++];
  c = nbt[(*pos)++];
  d = nbt[(*pos)++];
  e = nbt[(*pos)++];
  f = nbt[(*pos)++];
  g = nbt[(*pos)++];
  i = nbt[(*pos)++];
  return (a<<56) | (b << 48) | (c << 40) | (d << 32) | (e << 24) | (f << 16) | (g << 8) | i;
}

string get_length_prefixed_string(const unsigned char *nbt, int *pos) {
  uint16_t length = getint16(nbt, pos);
  string str = string((char*)(nbt + (*pos)), length);
  (*pos) += length;
  return str;
}

void parseNBT(const unsigned char *nbt, int size, const nbt_callbacks *cb, bool verbose) {
  int pos = 0;
  string name;
  unsigned char b;

  b = nbt[pos++];
  name = get_length_prefixed_string(nbt, &pos);
  parseTag(name, b, nbt, &pos, cb, verbose);
}

void parseTag(string name_in, uint8_t tag, const uint8_t *nbt, int *pos, const nbt_callbacks *cb, bool verbose) {
  switch (tag) {
  case 1: // 8bit int
  {
    uint8_t value = nbt[(*pos)++];
    if (verbose) printf("tag1 '%s' == %d / 0x%x\n", name_in.c_str(), value, value);
    if (cb && cb->tag1) cb->tag1(cb, name_in, value);
    break;
  }
  case 2: // 16bit int
  {
    uint16_t value = getint16(nbt, pos);
    if (verbose) printf("tag2 '%s' == %d / 0x%x\n", name_in.c_str(), value, value);
    if (cb && cb->tag2) cb->tag2(cb, name_in, value);
    break;
  }
  case 3: // 32bit int
  {
    uint32_t value = getint32(nbt, pos);
    if (verbose) printf("tag3 '%s' == %d / 0x%x\n", name_in.c_str(), value, value);
    if (cb && cb->tag3) cb->tag3(cb, name_in, value);
    break;
  }
  case 4: // 64bit int
  {
    uint64_t value = getint64(nbt, pos);
    if (verbose) printf("tag4 '%s' == %ld / 0x%lx\n", name_in.c_str(), value, value);
    break;
  }
  case 5: // 32bit float
  {
    uint32_t t = getint32(nbt, pos);
    float value = *((float*)(&t));
    if (verbose) printf("tag5 '%s' == %e\n", name_in.c_str(), value);
    break;
  }
  case 6: // 64bit double
  {
    uint64_t t = getint64(nbt, pos);
    double value = *((double*)(&t));
    if (verbose) printf("tag6 '%s' == %e\n", name_in.c_str(), value);
    break;
  }
  case 7: // byte array
  {
    uint32_t size = getint32(nbt, pos);
    if (verbose) printf("tag7 '%s' == byte array, size %d\n", name_in.c_str(), size);
    if (cb && cb->tag7) cb->tag7(cb, name_in, size, nbt + *pos);
    *pos += size;
    break;
  }
  case 8: // string
  {
    string value = get_length_prefixed_string(nbt, pos);
    if (verbose) printf("tag8 '%s' == '%s', pos 0x%x\n", name_in.c_str(), value.c_str(), *pos);
    if (cb && cb->tag8) cb->tag8(cb, name_in, value);
    break;
  }
  case 9: // tag list
  {
    uint8_t common_tag = nbt[(*pos)++];
    uint32_t elements = getint32(nbt, pos);
    if (verbose) printf("'%s' == %d elements of type %d\n", name_in.c_str(), elements, common_tag);
    char buf[10];
    for (unsigned int i=0; i<elements; i++) {
      snprintf(buf, 10, "/%d", i);
      string buf2 = buf;
      parseTag(name_in + buf2, common_tag, nbt, pos, cb, verbose);
    }
    break;
  }
  case 10:
  {
    //printf("BEGIN '%s'\n", name_in.c_str());
    while (true) {
      uint8_t b = nbt[(*pos)++];
      //printf("TAG %d\n", b);
      if (b == 0) break;
      string name = get_length_prefixed_string(nbt, pos);
      parseTag(name_in + "/" + name, b, nbt, pos, cb, verbose);
    }
    break;
  }
  case 11: // tag int array
  {
    uint32_t elements = getint32(nbt, pos);
    if (verbose) printf("'%s' = list of %d int32's\n", name_in.c_str(), elements);
    for (unsigned int i=0; i<elements; i++) {
      uint32_t value = getint32(nbt, pos);
      if (verbose) printf(" %d ", value);
    }
    if (verbose) puts("");
    break;
  }
  case 12: // long array
  {
    uint32_t elements = getint32(nbt, pos);
    if (verbose) printf("'%s' = list of %d int64's\n", name_in.c_str(), elements);
    for (unsigned int i=0; i<elements; i++) {
      uint64_t value = getint64(nbt, pos);
      if (verbose) printf(" 0x%lx", value);
    }
    if (verbose) puts("");
    break;
  }
  default:
    printf("'%s' unhandled tag %d\n", name_in.c_str(), tag);
    exit(-1);
    break;
  }
}


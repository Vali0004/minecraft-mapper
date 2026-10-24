// SPDX-License-Identifier: MPL-2.0
#include <assert.h>
#include <png.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "utils.h"

using namespace std;

uint64_t own_age;
bool quiet = false;

int load_png(const filesystem::path &path, Image &img) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  image.format = PNG_FORMAT_RGBA;

  if (png_image_begin_read_from_file(&image, path.c_str())) {
    //puts("load worked");
    //assert(PNG_IMAGE_SIZE(image) == (512*512*3));
    if (png_image_finish_read(&image, NULL, img.bitmap, 0, NULL) != 0) {
      //puts("finish worked");
      return 1;
    } else {
      puts("finish failed");
      return 0;
    }
  } else {
    //puts("load failed");
    //memset(buffer, 0, 512*512*3);
    return 0;
  }
}

void write_png(const filesystem::path &path, const Image &img) {
  create_directories(path.parent_path());
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  image.format = PNG_FORMAT_RGBA;
  image.width = 512;
  image.height = 512;

  if (!quiet) printf("output %s\n", path.c_str());
  int ret = png_image_write_to_file(&image, path.c_str(), false, img.bitmap, 512*4, NULL);
  if (!quiet) printf("ret %d\n", ret);
}

void to_file(const filesystem::path &path, const void *buffer, int size) {
  FILE *fd = fopen(path.c_str(), "wb");
  fwrite(buffer, size, 1, fd);
  fclose(fd);
}

string readFile(const filesystem::path &path) {
    string output;
    FILE* handle = fopen(path.c_str(), "r");
    if(!handle)
        return "";
    char buffer[100];
    while(size_t bytes = fread(buffer, 1, 99, handle)) {
        string part(buffer, bytes);
        output += part;
    }
    fclose(handle);
    return output;
}

int decompress(const void *input, int insize, void *output, int outsize) {
  z_stream strm = {
    .total_in = 0,
    .zalloc = NULL,
    .zfree = NULL,
    .opaque = NULL,
  };
  const uint8_t *in8 = (const uint8_t*)input;

  //printf("decompressing %5d bytes %2x %2x ", insize, in8[0], in8[1]);
  //int ret = inflateInit2(&strm, 32 + MAX_WBITS);
  int ret = inflateInit(&strm);
  assert(ret == Z_OK);

  inflateUndermine(&strm, true);

  strm.next_out = (unsigned char*)output;
  strm.avail_out = outsize;
  strm.total_out = 0;

  strm.next_in = (unsigned char*)input;
  strm.avail_in = insize;
  strm.total_in += insize;

  ret = inflate(&strm, Z_FINISH);
  //printf("decomp %d %ld availin: %5d ret: %d (%d consumed) %s\n", strm.avail_out, strm.total_out, strm.avail_in, ret, insize - strm.avail_in, strm.msg);

  inflateEnd(&strm);

  if (ret == Z_STREAM_END) {
    return strm.total_out;
  } else {
    return -1;
  }
}

uint64_t get_file_age(const filesystem::path &path) {
  struct stat statbuf;
  if (stat(path.c_str(), &statbuf) == -1) return 0;
  return statbuf.st_ctime;
}

uint64_t get_own_age() {
  char buffer[128];
  int len = readlink("/proc/self/exe", buffer, 120);
  buffer[len] = 0;
  return get_file_age(buffer);
}

bool need_update(const filesystem::path &input, const filesystem::path &output) {
  uint64_t output_age = get_file_age(output);
  //printf("%ld %ld\n", own_age, output_age);
  if (own_age > output_age) return true;

  uint64_t input_age = get_file_age(input);
  if (input_age > output_age) return true;
  return false;
}

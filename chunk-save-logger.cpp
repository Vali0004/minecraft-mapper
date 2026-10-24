// SPDX-License-Identifier: MPL-2.0
#include "inotify.h"
#include "region.h"

#include <arpa/inet.h>
#include <string>
#include <unistd.h>
#include "utils.h"

using namespace std;
using namespace filesystem;

class SaveWatcher : public InotifyWatcher {
public:
  SaveWatcher(filesystem::path savepath, int dimension, int x, int z);
  virtual void eventOccured(uint32_t mask);
private:
  int x, z;
  Region r;
  bool lastmod_loaded;
  uint32_t lastmods[32][32];
};

SaveWatcher::SaveWatcher(filesystem::path savepath, int dimension, int x, int z) : x(x), z(z), r(Region(savepath, dimension, x, z)), lastmod_loaded(false) {
}

void SaveWatcher::eventOccured(uint32_t mask) {
  if (mask & IN_MODIFY) {
    r.reload_header();
    int region_block_x = x << 9;
    int region_block_z = z << 9;
    for (int xoff=0; xoff<32; xoff++) {
      for (int zoff=0; zoff<32; zoff++) {
        uint32_t lastmod = ntohl(r.header->chunk_lastmods[zoff][xoff]);
        if (lastmod_loaded && (lastmod > lastmods[xoff][zoff])) {
          int block_x = region_block_x | (xoff << 4);
          int block_z = region_block_z | (zoff << 4);
          printf("%d / %d %d saved, last modified %d ago\n", r.dim, block_x, block_z, lastmod - lastmods[xoff][zoff]);
        }
        lastmods[xoff][zoff] = lastmod;
      }
    }
    lastmod_loaded = true;
  } else {
    printf("event 0x%x, for region %d,%d\n", mask, x, z);
  }
}

int main(int argc, char **argv) {
  filesystem::path savepath;

  int opt;
  while ((opt = getopt(argc, argv, "p:")) != -1) {
    switch (opt) {
    case 'p':
      savepath = optarg;
      break;
    }
  }

  Inotify in;
  //in.addWatch(savepath / "region", IN_CLOSE_WRITE | IN_CREATE | IN_OPEN | IN_MODIFY, NULL);
  for_each_dimension(savepath, [&in](std::filesystem::path dimDir, int dim) -> void {
    path regionDir = dimDir / "region";
    if (!exists(regionDir)) return;
    filesystem::directory_iterator iterator{ regionDir };
    for (auto &ent : iterator) {
      auto &p = ent.path();
      if (!ent.is_directory()) {
        auto coords = parse_region_name(p.stem().string());
        SaveWatcher *w = new SaveWatcher(dimDir, dim, coords.first, coords.second);
        in.addWatch(p, IN_CLOSE_WRITE | IN_CREATE | IN_OPEN | IN_MODIFY, w);
      }
    }
  });
  while (true) {
    in.blockUntilEvent();
  }
}

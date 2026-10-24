// SPDX-License-Identifier: MPL-2.0
#include "inotify.h"

#include <assert.h>
#include <dirent.h>
#include <iostream>
#include <unistd.h>

#include "event_loop.h"
#include "region.h"
#include "utils.h"

using namespace std;
using namespace filesystem;

class RegionWatcher : public InotifyWatcher {
public:
  RegionWatcher(string dimDir, int dim, int x, int z);
  virtual void eventOccured(uint32_t mask);
private:
  int x, z;
  string dimDir;
  int dim;
};

RegionWatcher::RegionWatcher(string dimDir, int dim, int x, int z) : dimDir(dimDir), dim(dim), x(x), z(z) {
}

void RegionWatcher::eventOccured(uint32_t mask) {
  if (mask & IN_MODIFY) printf("IN_MODIFY ");
  printf("dim %d region %d,%d, event 0x%x occured\n", dim, x, z, mask);
}

void event_loop(filesystem::path savedir, filesystem::path outdir) {
  Inotify in;

#if 0
  in.addWatch(savedir / "region", IN_CLOSE_WRITE | IN_CREATE | IN_OPEN, NULL);

  filesystem::directory_iterator iterator{ savedir / "region" };
  for (auto &ent : iterator) {
    auto &p = ent.path();
    if (!ent.is_directory()) {
      auto coords = parse_region_name(p.stem().string());
      RegionWatcher *w = new RegionWatcher(coords.first, coords.second);
      in.addWatch(p, IN_CLOSE_WRITE | IN_CREATE | IN_OPEN | IN_MODIFY, w);
    }
  }
#endif
  for_each_dimension(savedir, [&in](std::filesystem::path dimDir, int dim) -> void {
    path regionDir = dimDir / "region";
    if (!exists(regionDir)) return;
    in.addWatch(regionDir, IN_CLOSE_WRITE | IN_CREATE | IN_OPEN, NULL);
    filesystem::directory_iterator iterator{regionDir};
    for (auto &ent : iterator) {
      auto &p = ent.path();
      if (!ent.is_directory()) {
        auto coords = parse_region_name(p.stem().string());
        RegionWatcher *w = new RegionWatcher(dimDir.filename(), dim, coords.first, coords.second);
        in.addWatch(p, IN_CLOSE_WRITE | IN_CREATE | IN_OPEN | IN_MODIFY, w);
      }
    }
  });

  while (true) {
    in.blockUntilEvent();
  }
}

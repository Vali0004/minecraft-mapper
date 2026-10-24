// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <map>
#include <sys/inotify.h>

class InotifyWatcher {
public:
  virtual void eventOccured(uint32_t mask) = 0;
};

class Inotify {
public:
  Inotify();
  void addWatch(std::filesystem::path path, uint32_t events, InotifyWatcher *watcher);
  void blockUntilEvent();
private:
  int ifd;
  std::map<int, InotifyWatcher*> watchers;
};

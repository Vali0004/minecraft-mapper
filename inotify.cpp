// SPDX-License-Identifier: MPL-2.0
#include "inotify.h"

#include <assert.h>
#include <dirent.h>
#include <unistd.h>

using namespace std;

Inotify::Inotify() {
  ifd = inotify_init1(IN_CLOEXEC);
  assert(ifd >= 0);
}

void Inotify::addWatch(filesystem::path path, uint32_t events, InotifyWatcher *watcher) {
  int wd = inotify_add_watch(ifd, path.c_str(), events);
  watchers[wd] = watcher;
}

void Inotify::blockUntilEvent() {
  inotify_event *buffer = (inotify_event*)malloc(sizeof(inotify_event) + NAME_MAX);
  int size = read(ifd, buffer, sizeof(inotify_event) + NAME_MAX);

#if 0
  printf("\nwd: %d, mask: 0x%x, cookie: 0x%x, len: %d\n", buffer->wd, buffer->mask, buffer->cookie, buffer->len);
  if ((buffer->len > 0) && (buffer->len < NAME_MAX)) {
    printf("name: %s\n", buffer->name);
  }
  if (buffer->mask & IN_CLOSE_WRITE) puts("IN_CLOSE_WRITE");
  if (buffer->mask & IN_CREATE) puts("IN_CREATE");
  if (buffer->mask & IN_OPEN) puts("IN_OPEN");
  if (buffer->mask & IN_MODIFY) puts("IN_MODIFY");
#endif

  auto i = watchers.find(buffer->wd);
  if (i == watchers.end()) {
    puts("no watcher registered");
  } else {
    InotifyWatcher *w = i->second;
    if (w) {
      w->eventOccured(buffer->mask);
    } else {
      puts("watcher was NULL!");
    }
  }

  free(buffer);
}

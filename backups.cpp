// SPDX-License-Identifier: MPL-2.0
#include "backups.h"

#include <algorithm>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <zip.h>

#include "tile_db.h"

using namespace std;
using namespace filesystem;

// only the parts of the save the renderer reads
static bool wanted(const string &rel) {
  if (rel == "level.dat") return true;
  path p = rel;
  return p.extension() == ".mca" && p.parent_path().filename() == "region";
}

// extracts the world inside `zipfile` into `out`, the save root is wherever level.dat is
static bool extract_world(const path &zipfile, const path &out) {
  int err = 0;
  zip_t *z = zip_open(zipfile.c_str(), ZIP_RDONLY, &err);
  if (!z) {
    fprintf(stderr, "%s: cannot open zip (error %d)\n", zipfile.c_str(), err);
    return false;
  }

  zip_int64_t n = zip_get_num_entries(z, 0);
  string prefix;
  bool found = false;
  for (zip_int64_t i = 0; i < n && !found; i++) {
    string name = zip_get_name(z, i, 0);
    if (name == "level.dat" || (name.size() > 10 && name.ends_with("/level.dat"))) {
      prefix = name.substr(0, name.size() - strlen("level.dat"));
      found = true;
    }
  }
  if (!found) {
    fprintf(stderr, "%s: no level.dat\n", zipfile.c_str());
    zip_close(z);
    return false;
  }

  vector<char> buf(1 << 20);
  int files = 0;
  for (zip_int64_t i = 0; i < n; i++) {
    string name = zip_get_name(z, i, 0);
    if (!name.starts_with(prefix)) continue;
    string rel = name.substr(prefix.size());
    if (!wanted(rel) || rel.find("..") != string::npos) continue;

    path dst = out / rel;
    create_directories(dst.parent_path());
    zip_file_t *f = zip_fopen_index(z, i, 0);
    FILE *o = fopen(dst.c_str(), "wb");
    if (!f || !o) {
      fprintf(stderr, "%s: failed to extract %s\n", zipfile.c_str(), name.c_str());
      if (f) zip_fclose(f);
      if (o) fclose(o);
      zip_close(z);
      return false;
    }
    zip_int64_t got;
    while ((got = zip_fread(f, buf.data(), buf.size())) > 0) fwrite(buf.data(), 1, got, o);
    zip_fclose(f);
    fclose(o);
    if (got < 0) {
      fprintf(stderr, "%s: read error in %s\n", zipfile.c_str(), name.c_str());
      zip_close(z);
      return false;
    }
    files++;
  }
  zip_close(z);
  printf("%s: extracted %d files\n", zipfile.c_str(), files);
  return true;
}

// pterodactyl names backups YYYY-MM-DD-HH-MM-SS.zip in local time, fall back to the mtime
static int64_t backup_time(const path &zipfile) {
  tm t{};
  const char *end = strptime(zipfile.stem().c_str(), "%Y-%m-%d-%H-%M-%S", &t);
  if (end && *end == 0) {
    t.tm_isdst = -1;
    return mktime(&t);
  }
  struct stat st;
  return stat(zipfile.c_str(), &st) == 0 ? st.st_mtime : time(nullptr);
}

static void process_new(const path &dir, Db &db, RenderFn &render) {
  vector<path> zips;
  for (auto &ent : directory_iterator(dir)) {
    if (ent.is_regular_file() && ent.path().extension() == ".zip") zips.push_back(ent.path());
  }
  // oldest first so snapshot ids follow backup order
  sort(zips.begin(), zips.end(), [](const path &a, const path &b) { return backup_time(a) < backup_time(b); });

  for (auto &zipfile : zips) {
    const string note = zipfile.filename().string();
    if (has_snapshot_note(db, note)) continue;

    path work = temp_directory_path() / ("biome-thingy-" + to_string(getpid()));
    remove_all(work);
    path save = work / "save", out = work / "render";
    create_directories(out);
    if (extract_world(zipfile, save)) {
      render(save, out);
      ingest(db, out, note.c_str(), backup_time(zipfile));
    }
    remove_all(work);
  }
}

void backup_loop(const path &dir, const string &db_path, RenderFn render) {
  Db db(db_path);
  int ifd = inotify_init1(IN_CLOEXEC);
  // a finished upload is either closed in place or renamed into the directory
  if (ifd < 0 || inotify_add_watch(ifd, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
    perror("inotify");
    exit(1);
  }

  while (true) {
    process_new(dir, db, render);
    char buf[4096];
    if (read(ifd, buf, sizeof(buf)) <= 0) {
      perror("inotify read");
      exit(1);
    }
    // the backup tool may still be writing other files, let the burst settle
    sleep(5);
    int fl = fcntl(ifd, F_GETFL);
    fcntl(ifd, F_SETFL, fl | O_NONBLOCK);
    while (read(ifd, buf, sizeof(buf)) > 0) {}
    fcntl(ifd, F_SETFL, fl);
  }
}

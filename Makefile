# SPDX-License-Identifier: MPL-2.0
PREFIX ?= /usr/local
BINS = biome-thingy chunk-save-logger pollution-exporter tile-history

all: $(BINS)

install: $(BINS)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/bin $^

.PHONY: all install viewer

# the web viewer, needs emscripten (`nix build .#map-viewer`)
EMXX ?= em++
viewer: web/viewer.js web/index.html

web/viewer.js: viewer/viewer.cpp
	mkdir -p web
	$(EMXX) -o $@ $< -std=c++20 -O2 -Wall -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH \
		-sEXPORTED_FUNCTIONS=_main,_viewer_resize,_viewer_set_snapshot,_viewer_set_dim,_viewer_goto,_viewer_tile_done,_viewer_can_3d,_viewer_set_3d,_viewer_textures_ready,_viewer_set_camera,_viewer_set_scale,_viewer_set_layer,_viewer_set_xray \
		-sEXPORTED_RUNTIME_METHODS=ccall

web/index.html: viewer/index.html
	mkdir -p web
	cp $< $@

biome-thingy: biome-thingy.o nbt.o utils.o chunk.o zooms.o image.o event_loop.o region.o inotify.o backups.o tile_db.o
	$(CXX) -o $@ $^ -Wall -lz -lpng -lzip -lsqlite3 -g -pthread

chunk-save-logger: chunk-save-logger.o inotify.o region.o
	$(CXX) -o $@ $^ -Wall -g

pollution-exporter: pollution-exporter.o utils.o metrics_http.o nbt_tree.o
	$(CXX) -o $@ $^ -Wall -g -lz -lpng -pthread

tile-history: tile-history.o utils.o tile_db.o metrics_http.o
	$(CXX) -o $@ $^ -Wall -g -lz -lpng -lsqlite3 -pthread

%.o: %.cpp
	$(CXX) -o $@ $< -Wall -c -std=c++20 -pthread -g -O3 -fno-inline-functions

%.deps.o: %.cpp
	$(CXX) -o $@ -M $<

biome-thingy.o: biome-thingy.cpp backups.h nbt.h utils.h chunk.h section.h zooms.h image.h event_loop.h region.h
chunk-save-logger.o: chunk-save-logger.cpp inotify.h region.h
chunk.o: chunk.cpp chunk.h section.h
event_loop.o: event_loop.cpp event_loop.h region.h inotify.h
image.o: image.cpp image.h
inotify.o: inotify.cpp
nbt.o: nbt.cpp nbt.h
region.o: region.cpp region.h
utils.o: utils.cpp utils.h image.h
zooms.o: zooms.cpp zooms.h utils.h image.h
metrics_http.o: metrics_http.cpp metrics_http.h
pollution-exporter.o: pollution-exporter.cpp utils.h metrics_http.h nbt_tree.h
nbt_tree.o: nbt_tree.cpp nbt_tree.h utils.h
tile-history.o: tile-history.cpp utils.h tile_db.h metrics_http.h
tile_db.o: tile_db.cpp tile_db.h utils.h
backups.o: backups.cpp backups.h tile_db.h

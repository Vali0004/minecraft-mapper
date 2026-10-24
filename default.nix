# SPDX-License-Identifier: MPL-2.0
{ lib, stdenv, zlib, libpng, nlohmann_json, sqlite, libzip, emscripten }:

let
  mkApp = { pname, buildInputs ? [ ], extraInstall ? "", description }:
    stdenv.mkDerivation {
      inherit pname buildInputs;
      version = "0.1.0";
      src = lib.cleanSource ./.;
      enableParallelBuilding = true;
      buildFlags = [ pname ];
      installPhase = ''
        runHook preInstall
        install -Dm755 ${pname} $out/bin/${pname}
        ${extraInstall}
        runHook postInstall
      '';
      meta = {
        license = lib.licenses.mpl20;
        inherit description;
        mainProgram = pname;
        platforms = lib.platforms.linux;
      };
    };
in
{
  biome-thingy = mkApp {
    pname = "biome-thingy";
    buildInputs = [ zlib libpng nlohmann_json libzip sqlite ];
    extraInstall = ''
      install -Dm644 -t $out/share/biome-thingy colors.json transparent_blocks.json
    '';
    description = "Render minecraft world maps";
  };

  chunk-save-logger = mkApp {
    pname = "chunk-save-logger";
    description = "Log chunk saves of a minecraft world";
  };

  pollution-exporter = mkApp {
    pname = "pollution-exporter";
    buildInputs = [ zlib libpng nlohmann_json ];
    description = "Prometheus exporter for GTNH pollution";
  };

  tile-history = mkApp {
    pname = "tile-history";
    buildInputs = [ zlib libpng sqlite ];
    description = "Keep a history of rendered map tiles in sqlite";
  };

  map-viewer = stdenv.mkDerivation {
    pname = "map-viewer";
    version = "0.1.0";
    src = lib.cleanSource ./.;
    nativeBuildInputs = [ emscripten ];
    buildPhase = ''
      runHook preBuild
      # emscripten wants a writable cache, start from the prebuilt one
      cp -r ${emscripten}/share/emscripten/cache $TMPDIR/em-cache
      chmod -R u+w $TMPDIR/em-cache
      export EM_CACHE=$TMPDIR/em-cache HOME=$TMPDIR
      make viewer
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm644 -t $out/share/map-viewer web/index.html web/viewer.js web/viewer.wasm
      runHook postInstall
    '';
    meta = {
      license = lib.licenses.mpl20;
      description = "WebGL viewer for tile-history, serve with `tile-history serve -w`";
      platforms = lib.platforms.all;
    };
  };
}

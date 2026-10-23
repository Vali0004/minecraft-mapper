{ lib, stdenv, zlib, libpng, nlohmann_json, sqlite }:

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
        inherit description;
        mainProgram = pname;
        platforms = lib.platforms.linux;
      };
    };
in
{
  biome-thingy = mkApp {
    pname = "biome-thingy";
    buildInputs = [ zlib libpng nlohmann_json ];
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
}

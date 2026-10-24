# SPDX-License-Identifier: MPL-2.0
{
  outputs = { self, nixpkgs }:
  let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
    apps' = pkgs.lib.filterAttrs (_: pkgs.lib.isDerivation) (pkgs.callPackage ./. { });
  in
  {
    packages.${system} = apps' // {
      default = pkgs.symlinkJoin {
        name = "minecraft-mapper";
        paths = builtins.attrValues apps';
      };
    };

    apps.${system} = builtins.mapAttrs (name: drv: {
      type = "app";
      program = "${drv}/bin/${name}";
    }) (removeAttrs apps' [ "map-viewer" ]);

    devShells.${system}.default = pkgs.mkShell {
      inputsFrom = builtins.attrValues apps';
      nativeBuildInputs = with pkgs; [ qpdf nodejs valgrind ];
    };
  };
}

#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# builds texture-dump.jar against the forge and lwjgl jars of a prism launcher install
set -euo pipefail
cd "$(dirname "$0")"
LIBS=${PRISM_LIBS:-$HOME/.local/share/PrismLauncher/libraries}
CP="$LIBS/net/minecraftforge/forge/1.7.10-10.13.4.1614-1.7.10/forge-1.7.10-10.13.4.1614-1.7.10-universal.jar"
CP="$CP:$LIBS/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar:$LIBS/org/lwjgl/lwjgl-opengl/3.3.3/lwjgl-opengl-3.3.3.jar"
rm -rf build
mkdir -p build
javac --release 8 -nowarn -cp "$CP" -d build $(find src -name '*.java')
cp mcmod.info build/
jar cf texture-dump.jar -C build .
echo "built $(pwd)/texture-dump.jar"

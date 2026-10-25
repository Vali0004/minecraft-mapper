#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# builds server-metrics.jar against the forge jar of a prism launcher install
set -euo pipefail
cd "$(dirname "$0")"
LIBS=${PRISM_LIBS:-$HOME/.local/share/PrismLauncher/libraries}
CP="$LIBS/net/minecraftforge/forge/1.7.10-10.13.4.1614-1.7.10/forge-1.7.10-10.13.4.1614-1.7.10-universal.jar"
CP="$CP:$(ls $LIBS/com/google/guava/guava/17.0/guava-17.0.jar)"
rm -rf build
mkdir -p build
javac --release 8 -nowarn -cp "$CP" -d build $(find src -name '*.java')
cp mcmod.info build/
jar cf server-metrics.jar -C build .
echo "built $(pwd)/server-metrics.jar"

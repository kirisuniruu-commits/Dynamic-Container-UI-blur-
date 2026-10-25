#!/usr/bin/env bash
set -euo pipefail
BUILD="${1:-build}"
NAME="dynamic-container-ui-blur"
SO="$BUILD/out/arm64-v8a/libdynamic_container_ui_blur.so"
[ -f "$SO" ] || { echo "missing $SO"; exit 1; }

rm -rf dist
mkdir -p "dist/$NAME/resources"
cp manifest.json "dist/$NAME/"
cp "$SO" "dist/$NAME/"
cp resources/*.json "dist/$NAME/resources/"
( cd "dist/$NAME" && zip -qr "../$NAME.levipack" . )
echo "built dist/$NAME.levipack"
unzip -l "dist/$NAME.levipack"

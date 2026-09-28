#!/bin/sh
# Build the Godot 4.6 linux-arm64 release engine + the godot-libpd extension
# .so inside Docker (native linux/arm64 on Apple Silicon).
#
#   ./engine_build/build-knulli-docker.sh
#
# The image is built once from the small engine_build/ context and cached
# (override with IMAGE=...). The repo is then mounted at /src and built in
# place; artifacts land in bin/ and extension/build/ (both gitignored).
#
# Progress log: dist/knulli-engine-build.log
set -e
cd "$(dirname "$0")/.."

IMAGE=${IMAGE:-godot-libpd-knulli-builder}
LOG=dist/knulli-engine-build.log
mkdir -p dist

echo "== Building builder image (cached after first run)"
docker build --platform linux/arm64 -f engine_build/Dockerfile.knulli -t "$IMAGE" engine_build

echo "== Running in-place build (log: $LOG)"
docker run --rm --platform linux/arm64 \
    -v "$PWD":/src \
    "$IMAGE" 2>&1 | tee "$LOG"

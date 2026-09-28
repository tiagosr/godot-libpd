#!/bin/sh
# In-container build (repo root mounted at /src). Produces, in place:
#   1. Godot 4.6 linux-arm64 RELEASE engine -> /src/bin/godot.linuxbsd.arm64
#   2. godot-libpd extension (linux arm64)   -> /src/extension/build/linux/libgodot_libpd.so
#
# The repo must be on the 4.6 line (branch gdext-libpd, based on the
# 4.6-stable tag). Both output trees are gitignored.
set -e
cd /src

echo "== HEAD: $(git rev-parse --short HEAD) (branch: $(git branch --show-current))"

echo "== [1/2] Godot 4.6 linux-arm64 release engine (scons, -j$(nproc))"
# 4.6 target values: editor | template_release | template_debug (no bare "release")
scons platform=linux target=template_release arch=arm64 dev_build=false -j"$(nproc)"
# 4.6 names template binaries godot.<platform>.<target>.<arch>
file bin/godot.linuxbsd.template_release.arm64

echo "== [2/2] godot-libpd extension (cmake, plain aarch64 host)"
cd extension
./build.sh --linux-arm64
file build/linux/libgodot_libpd.so

echo "== DONE"

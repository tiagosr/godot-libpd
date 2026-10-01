#!/bin/sh
# In-container build (repo root mounted at /src). Produces, in place:
#   1. Godot 4.6 linux-arm64 RELEASE engine -> /src/bin/godot.linuxbsd.template_release.arm64
#   2. godot-libpd extension (linux arm64)   -> /src/extension/build/linux/libgodot_libpd.so
#
# The repo must be on the 4.6 line (branch gdext-libpd, based on the
# 4.6-stable tag). Both output trees are gitignored.
set -e
cd /src

echo "== HEAD: $(git rev-parse --short HEAD) (branch: $(git branch --show-current))"

echo "== [1/2] Godot 4.6 linux-arm64 release engine (scons, -j$(nproc))"
# 4.6 target values: editor | template_release | template_debug (no bare "release")
# fbdev=yes builds the framebuffer display server (bare-Linux handhelds).
# x11/wayland stay available so the host fallback path keeps working.
# sdl=no: CRITICAL on bare-Linux handhelds. With the default (sdl=yes),
# OS_LinuxBSD::initialize_joypads() also creates a JoypadSDL, whose
# built-in SDL evdev backend opens the SAME /dev/input devices as the
# fbdev display server's own evdev thread. Both readers feed the Input
# singleton -> every physical D-pad/button press is delivered to scripts
# TWICE (two distinct InputEvent objects). The fbdev DS is the sole input
# source for these devices, so the SDL joypad driver must be out of the build.
scons platform=linux target=template_release arch=arm64 dev_build=false fbdev=yes sdl=no -j"$(nproc)"
# 4.6 names template binaries godot.<platform>.<target>.<arch>
file bin/godot.linuxbsd.template_release.arm64

echo "== [2/2] godot-libpd extension (cmake, plain aarch64 host)"
cd extension
./build.sh --linux-arm64
file build/linux/libgodot_libpd.so

echo "== DONE"

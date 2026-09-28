#!/bin/sh
# Package the Knulli bundle(s): dist/knulli/<board>/ containing the exported
# linux-arm64 test app (custom-engine binary + pck + extension .so) and a
# godot.sh launcher. Content is board-agnostic (a133 = Trimui Brick,
# h700 = RG35XX*/RGCubeXX*); the board is just a label for the user.
#
#   ./engine_build/package-knulli.sh [a133|h700|all]     (default: all)
#
# Prereqs (Task 8 + Task 9 step 1):
#   - bin/godot.linuxbsd.template_release.arm64 exists (./build-knulli-docker.sh)
#   - it is installed as the 4.6.2 linux_release.arm64 export template
#     (backup of the official file: dist/linux_release.arm64.official-backup)
#     TPL=<template dir> ./package-knulli.sh re-installs it first.
#
# Copy the whole dist/knulli/<board>/ folder to the device (e.g.
# roms/ports/) and run ./godot.sh.
set -e
cd "$(dirname "$0")/.."

BOARD=${1:-all}
GODOT_BIN=${GODOT_BIN:-/Applications/Godot.app/Contents/MacOS/Godot}
APP_NAME=${APP_NAME:-test}
# Directory name the bundle gets copied to on the device (roms/ports/<dir>/);
# used by the thin port-level launcher.
PORT_APP_DIR=${PORT_APP_DIR:-godot-libpd}
ENGINE=bin/godot.linuxbsd.template_release.arm64
[ -x "$ENGINE" ] || { echo "error: $ENGINE missing - run ./engine_build/build-knulli-docker.sh first" >&2; exit 1; }

build_id() { file "$1" | grep -o "BuildID[sha1]=[a-f0-9]*" | head -1; }
WANT_ID=$(build_id "$ENGINE")

install_template() {
  local tpl_dir
  tpl_dir=$(ls -1d "$HOME/Library/Application Support/Godot/export_templates/"*"/linux_release.arm64" 2>/dev/null | head -1 | xargs -I{} dirname {})
  [ -n "$tpl_dir" ] || { echo "error: no Godot export template dir found (set TPL=/path/to/template/dir)" >&2; return 1; }
  cp "$ENGINE" "$tpl_dir/linux_release.arm64"
  echo "installed custom template: $tpl_dir/linux_release.arm64"
}
if [ -n "${TPL:-}" ]; then install_template; fi

pack() {
  board=$1
  out="dist/knulli/$board"
  echo "== packing $out"
  rm -rf "$out"; mkdir -p "$out"

  "$GODOT_BIN" --headless --path test_project --export-release "Linux" >/dev/null
  cp dist/linux/godot-libpd-test "$out/$APP_NAME"
  cp dist/linux/godot-libpd-test.pck "$out/$APP_NAME.pck"
  cp dist/linux/libgodot_libpd.so "$out/libgodot_libpd.so"
  chmod +x "$out/$APP_NAME"

  file "$out/$APP_NAME" | grep -q "ARM aarch64" || { echo "ERROR: $out/$APP_NAME is not aarch64 - Linux preset architecture must be arm64" >&2; exit 1; }
  GOT_ID=$(build_id "$out/$APP_NAME")
  [ "$GOT_ID" = "$WANT_ID" ] || { echo "ERROR: exported binary is not the custom engine ($GOT_ID != $WANT_ID) - install the custom template (Task 9 step 1)" >&2; exit 1; }

  printf '#!/bin/sh\nexec "$(dirname "$0")/%s" "$@"\n' "$APP_NAME" > "$out/godot.sh"
  chmod +x "$out/godot.sh"

  # Thin port-level entry (copy to roms/ports/<PORT_APP_DIR>.sh alongside the
  # bundle dir). Deliberately free of nested $(...) substitutions: on the
  # A133 (kernel 4.9) a dash stuck in a nested subshell with a torn-down adb
  # PTY spins at 100% CPU and becomes unkillable (device needs a reboot).
  printf '#!/bin/sh\n# Thin port entry: place at roms/ports/%s.sh next to the app dir.\ncd "$(dirname "$0")/%s" || exit 1\nexec ./godot.sh "$@"\n' "$PORT_APP_DIR" "$PORT_APP_DIR" > "$out/port-launcher.sh"
  chmod +x "$out/port-launcher.sh"

  ls -la "$out"
  echo "== $out ready (copy to device roms/ports/$PORT_APP_DIR/, then port-launcher.sh to roms/ports/$PORT_APP_DIR.sh)"
}

case "$BOARD" in
  a133) pack a133 ;;
  h700) pack h700 ;;
  all)  pack a133; pack h700 ;;
  *) echo "usage: $0 [a133|h700|all]" >&2; exit 1 ;;
esac

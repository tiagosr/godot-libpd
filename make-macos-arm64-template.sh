#!/bin/sh
# Regenerate an arm64-only macOS export template from the official (universal)
# one, so the macOS preset can export a lean arm64 binary. The official Godot
# 4.6.2 macos.zip ships only `godot_macos_{debug,release}.universal`; the
# exporter wants `godot_macos_{debug,release}.arm64` for an arm64 build, so we
# `lipo -thin arm64` the slices and repackage.
#
#   ./make-macos-arm64-template.sh [official_macos.zip] [out.zip]
#
# Defaults:
#   official = ~/Library/Application Support/Godot/export_templates/<ver>/macos.zip
#   out      = test_project/export_templates/macos-arm64.zip
set -e
cd "$(dirname "$0")"

SRC="${1:-}"
OUT="${2:-test_project/export_templates/macos-arm64.zip}"

if [ -z "$SRC" ]; then
  base="$HOME/Library/Application Support/Godot/export_templates"
  # newest version dir that has macos.zip
  SRC="$(ls -1d "$base"/*/macos.zip 2>/dev/null | sort | tail -1)"
fi
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "error: official macos.zip not found. Pass it as the first argument." >&2
  exit 1
fi
echo "source: $SRC"
echo "output: $OUT"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
unzip -q "$SRC" -d "$tmp"

macos_dir="$tmp/macos_template.app/Contents/MacOS"
for kind in release debug; do
  uni="$macos_dir/godot_macos_${kind}.universal"
  arm="$macos_dir/godot_macos_${kind}.arm64"
  if [ -f "$uni" ]; then
    lipo "$uni" -thin arm64 -output "$arm"
    rm -f "$uni"
  fi
done

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
( cd "$tmp" && zip -q -r -y "$OUT" macos_template.app )

echo "done. Contents:"
unzip -l "$OUT"

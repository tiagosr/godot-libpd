#!/bin/sh
# ES port entry for godot-libpd (Trimui Brick / Knulli).
#
# Execs the tmpfs-staged copy when present (staged at boot by
# /etc/init.d/S99godotstage) - the A133's /userdata (exfat) wedges
# minutes after boot and any read there hangs the whole launch.
# Falls back to the exfat bundle when no stage exists yet (fresh
# install, first launch after deploy).
#
# Kept free of nested shell substitutions (A133 dash quirk).

D=/tmp/godot-app
[ -x "$D/test" ] || D=/userdata/roms/ports/godot-libpd
cd "$D" || exit 1
echo "=== LAUNCH $(date) src=$D ===" >> /tmp/godot_launch.log 2>&1
./test "$@" >> /tmp/godot_run.log 2>&1
echo "=== EXIT $? at $(date) src=$D ===" >> /tmp/godot_launch.log 2>&1

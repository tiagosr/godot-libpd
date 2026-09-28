#!/bin/sh
# Reference on-device smoke loop (A133/H700, Knulli).
#
# Copy to the device (e.g. roms/ports/godot-libpd/loop_smoke.sh) and run it
# SYNCHRONOUSLY through adb with a generous host-side timeout — never let
# the adb session die mid-run (see engine_build/README.md, firmware caveats):
#
#   timeout 120 adb shell "sh /userdata/roms/ports/godot-libpd/loop_smoke.sh"
#
# Writes /tmp/smoke_results.txt (tmpfs). PD_DBG=1 captures a merged
# worker+main-thread timeline in /tmp/godot_libpd_<id>.log for the last run,
# so a hung run leaves a readable timeline.
PORTS_DIR=${PORTS_DIR:-/userdata/roms/ports}
LAUNCHER=${LAUNCHER:-godot-libpd.sh}
RUNS=${RUNS:-10}
RUN_TIMEOUT=${RUN_TIMEOUT:-15}

cd "$PORTS_DIR" || exit 1
: > /tmp/smoke_results.txt
i=1
while [ "$i" -le "$RUNS" ]; do
	rm -f /tmp/godot_libpd_*.log /tmp/smoke_progress.log
	PD_DBG=1 timeout -k 3 "$RUN_TIMEOUT" "./$LAUNCHER" --headless --smoke < /dev/null
	rc=$?
	nlogs=$(ls /tmp/godot_libpd_*.log 2>/dev/null | wc -l)
	spl=$(ls /tmp/smoke_progress.log 2>/dev/null | wc -l)
	echo "run $i: rc=$rc wlog_files=$nlogs smokeprog_present=$spl" >> /tmp/smoke_results.txt
	if [ "$rc" -ne 0 ]; then
		{
			echo "== HANG in run $i; /tmp timelines =="
			echo "-- smoke_progress.log --"
			cat /tmp/smoke_progress.log 2>/dev/null
			echo "-- godot_libpd_*.log --"
			for f in /tmp/godot_libpd_*.log; do
				echo "== $f =="
				cat "$f" 2>/dev/null
			done
		} >> /tmp/smoke_results.txt
		break
	fi
	i=$((i + 1))
done
echo "loop done" >> /tmp/smoke_results.txt
cat /tmp/smoke_results.txt

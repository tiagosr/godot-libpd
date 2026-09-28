# Knulli (Allwinner handhelds) — Godot 4.6 engine + godot-libpd builds

Targets: **H700** (RG35XX*, RGCubeXX*, kernel 4.9.170) and **A133** (Trimui
Brick, kernel 4.9.191), running Knulli CFW (Batocera fork). All builds run
natively as `linux/arm64` in Docker on the Apple Silicon host (no
cross-toolchain).

## Flow

```sh
# 1. Engine (4.6-stable line) + linux extension .so   [~30 min first run]
./engine_build/build-knulli-docker.sh
#    -> bin/godot.linuxbsd.template_release.arm64      (ELF aarch64 PIE)
#    -> extension/build/linux/libgodot_libpd.so        (ELF aarch64)

# 2. Install the custom binary as the 4.6.2 linux.arm64 export template
#    (once; official file is backed up to
#     dist/linux_release.arm64.official-backup first):
cp dist/linux_release.arm64.official-backup <template dir>/  # restore if ever needed
# package-knulli.sh does this automatically when TPL=<template dir> is set.

# 3. Package per-board bundles
./engine_build/package-knulli.sh all
#    -> dist/knulli/a133/{test, test.pck, libgodot_libpd.so, godot.sh, port-launcher.sh}
#    -> dist/knulli/h700/{...identical...}
#    (script verifies aarch64 + that the exported binary is the custom build)

# 4. Host-side smoke (docker, replaces the on-device check while no device
#    is handy):
docker run --rm --platform linux/arm64 -v "$PWD/dist/knulli/a133":/app \
    godot-libpd-knulli-builder /bin/sh -c "cd /app && ./godot.sh --headless --smoke"
#    expect SMOKE_OK
```

## Device test (A133 / Trimui Brick)

Copy the whole `dist/knulli/a133/` folder to the device as
`roms/ports/godot-libpd/`, and copy `port-launcher.sh` to
`roms/ports/godot-libpd.sh` (thin wrapper → in-bundle `godot.sh`).

### Verified on-device (2026-09-28)

`./godot-libpd.sh --headless --smoke` → **SMOKE_OK in ~1.4s**, repeated
10/10 with no hangs and no device instability. The smoke test exercises:
engine load, extension load, pd init, patch load from the pck-extracted
file, DSP start, rendered audio blocks, MIDI note → pd print round-trip,
multi-instance spawn/kill, clean teardown (worker join < 5 ms).

GUI mode needs a display server (X11/Wayland) — Knulli ships neither
(ES uses SDL2/fbcon), so on-device verification is headless until a
display backend is chosen (SDL2/fbcon, DRM, or a compositor such as MIR;
see spec §9).

First thing to check on failure: `ldd --version` on-device. **The current
builds need glibc >= 2.35** (max `GLIBC_2.35` symbol in both the engine and
the .so, built on Ubuntu 22.04). Knulli's Buildroot glibc is 2.40 — OK.

### On-device hang diagnostics (PD_DBG)

The extension and smoke test carry env-gated debug logging (zero overhead
when unset). To capture a full worker+main-thread timeline when something
stalls:

```sh
PD_DBG=1 timeout -k 3 15 ./godot-libpd.sh --headless --smoke < /dev/null
# -> /tmp/godot_libpd_<id>.log  (per-instance merged timeline, tmpfs)
# -> /tmp/smoke_progress.log    (smoke-test step log, tmpfs)
```

`PD_DBG_DIR=<dir>` overrides the log dir. Note: on exfat (`/userdata`)
file sizes are only committed at `close()`, so a killed run leaves
0-byte logs — always log to `/tmp` (tmpfs) when hunting hangs.

Reference loop: `engine_build/tools/loop_smoke.sh`.

### A133 firmware caveats (kernel 4.9) — read before on-device testing

- **Never let the host-side adb session die mid-run.** When adb is cut, the
  device-side `timeout` wrapper dies and orphans its child; a dash stuck in
  a `$(...)` subshell with a torn-down PTY then spins at 100% CPU and can
  become **unkillable (even SIGKILL)** — the kernel must be rebooted.
  Observed on this device: spin started in the nested-substitution launcher,
  survived 25+ min, ignored `kill -9`, kernel stack showed corrupted
  `xradio_mac` (wifi driver) frames. The accumulated load from such
  spinners is what tripped the watchdog reboots seen during testing.
- Keep the launchers **free of nested `$(...)` substitutions** (the shipped
  launchers are one level deep on purpose).
- Run device test loops **synchronously through adb** with a generous
  host-side timeout, or detached via `setsid sh … &` (a `setsid` launch can
  race with a concurrent file write on exfat — write the script first,
  verify it, then launch).
- Baseline load on the Brick is ~2–3 (EmulationStation + wpa_supplicant +
  stats daemon); a runaway app is visible as load climbing well past that.

### glibc fallback ladder (only if needed)

1. Rebuild the engine with a static libc in the same Docker image:
   `scons platform=linux target=template_release arch=arm64 dev_build=false static_libc=true`
   (repackage as usual).
2. If static libc fails, lower the glibc floor via the build and rebuild.
3. Re-run the device test after each step and record the outcome here.

**Outcome: verified 2026-09-28 — headless smoke passes on-device (10/10); glibc 2.40 present, no fallback needed.**

## Notes

- Godot 4.6 scons has no bare `target=release` — use `target=template_release`
  (binary is named `godot.<platform>.template_release.<arch>`).
- The Docker image needs pip `cmake>=3.25,<4` (Ubuntu 22.04's 3.22 is too old
  for libpd) and pip `scons` (>= 4.2).
- The extension must be built with `GODOTCPP_TARGET=template_release`
  (see spec §8 — a DEBUG_ENABLED godot-cpp corrupts in release templates);
  `extension/build.sh` sets this.
- The Linux export preset must stay at `binary_format/architecture="arm64"`.

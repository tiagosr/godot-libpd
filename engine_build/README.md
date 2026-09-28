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
#    -> dist/knulli/a133/{test, test.pck, libgodot_libpd.so, godot.sh}
#    -> dist/knulli/h700/{...identical...}
#    (script verifies aarch64 + that the exported binary is the custom build)

# 4. Host-side smoke (docker, replaces the on-device check while no device
#    is handy):
docker run --rm --platform linux/arm64 -v "$PWD/dist/knulli/a133":/app \
    godot-libpd-knulli-builder /bin/sh -c "cd /app && ./godot.sh --headless --smoke"
#    expect SMOKE_OK
```

## Device test (A133 / Trimui Brick)

Copy the whole `dist/knulli/a133/` folder to the device (e.g.
`roms/ports/`), run `./godot.sh`. Expected: test app opens; Load Patch →
Start DSP → Send Test Note produces an audible sine out of the 3.5 mm jack,
prints appear in the log, +Instance works.

First thing to check on failure: `ldd --version` on-device. **The current
builds need glibc >= 2.35** (max `GLIBC_2.35` symbol in both the engine and
the .so, built on Ubuntu 22.04). If the firmware ships older glibc, the
launcher dies with `GLIBC_2.3x not found`.

### glibc fallback ladder (only if needed)

1. Rebuild the engine with a static libc in the same Docker image:
   `scons platform=linux target=template_release arch=arm64 dev_build=false static_libc=true`
   (repackage as usual).
2. If static libc fails, lower the glibc floor via the build and rebuild.
3. Re-run the device test after each step and record the outcome here.

**Outcome: (pending first device test)**

## Notes

- Godot 4.6 scons has no bare `target=release` — use `target=template_release`
  (binary is named `godot.<platform>.template_release.<arch>`).
- The Docker image needs pip `cmake>=3.25,<4` (Ubuntu 22.04's 3.22 is too old
  for libpd) and pip `scons` (>= 4.2).
- The extension must be built with `GODOTCPP_TARGET=template_release`
  (see spec §8 — a DEBUG_ENABLED godot-cpp corrupts in release templates);
  `extension/build.sh` sets this.
- The Linux export preset must stay at `binary_format/architecture="arm64"`.

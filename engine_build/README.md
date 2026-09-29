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

### GUI mode: `DisplayServerFbdev` (the vendor DC display path)

Knulli ships no X11/Wayland. The Brick's panel is scanned out by the
vendor **PowerVR display controller** (`pvrsrvkm`/`disp`), *not* by
`/dev/fb0`. A process claims the panel by creating an **EGL window
surface** — the vendor NULL-window-system backend
(`libpvrNULL_WSEGL`, `WSEGL_CreateWindowDrawable`, logs
`MALI_CreateWindow`) allocates the DC buffer — and then presenting with
**`eglSwapBuffers()`**. This is exactly what EmulationStation and the
SDL2-based ports (e.g. trackerjolo) do; it is what the `fbdev` display
server in `platform/linuxbsd/fbdev/` implements:

- One fixed fullscreen window (1024x768; geometry read from `/dev/fb0`,
  which is opened **read-only** — never written).
- GLES3 compatibility rasterizer renders into the window surface's
  default framebuffer; `swap_buffers()` presents via `eglSwapBuffers()`
  and paces to ~60fps (`GODOT_FBDEV_FPS`).
- `eglCreateWindowSurface(display, cfg, /*window=*/0, NULL)` is the claim
  call; `eglCreatePbufferSurface` does *not* claim the display (proven
  on-device: PBuffer+swap renders into a buffer nobody scans out).
- The old `glReadPixels`→fb0 blit survives behind `GODOT_FBDEV_FB0=1`
  for diagnostics only. Writing fb0 while ES runs only produces flicker
  and is not scanned out once a port holds the DC.
- Registered **first** in `OS_LinuxBSD`, so it is the default driver;
  without `/dev/fb0` (desktop hosts) `create` fails and the engine falls
  back to x11/wayland.
- The project must request the Compatibility renderer on linuxbsd
  (`rendering_method.linuxbsd=gl_compatibility` in `project.godot`);
  Vulkan is broken on the Brick (ICD present, `vkCreateInstance` fails).

Build with `fbdev=yes` (already in `build-knulli-engine.sh`).

Verified on-device (2026-09-29): ES menu → port launch → Godot splash →
test UI (stable, no flicker) → engine exit → ES menu restored. The
probe suite in `engine_build/tools/` (`drm_probe.c`, `egl_probe.c`,
`vk_probe.c`, `fbtest.c`, `swaptest.c`, `winstest.c`) documents how this
was established: fb0 is a separate fb that ES also paints; the DC is
the real panel path.

First thing to check on failure: `ldd --version` on-device. **The current
builds need glibc >= 2.35** (max `GLIBC_2.35` symbol in both the engine and
the .so, built on Ubuntu 22.04). Knulli's Buildroot glibc is 2.40 — OK.

### Input: evdev (the firmware uinput gamepad)

The Brick's controller is a **uinput device** created by the
`trimui_inputd` daemon (polls GPIO → `/dev/uinput` → `/dev/input/event3`,
reported as `TRIMUI Brick Controller`, id `045e:028e:0114`).
`DisplayServerFbdev` reads it with a dedicated thread:

- At DS construction the thread scans `/dev/input/event*`, opens every
device with key/abs capability (`EVIOCGBIT` — note: this ioctl returns
the **byte count copied**, not 0; test `>= 0`, and give the KEY mask its
full 12 longs or older kernels reject it), and `poll()`s them.
- **Events are never dispatched from the evdev thread.** They are queued
(mutex-protected) and delivered to `Input::parse_input_event()` from
`DisplayServer::process_events()` — main thread, once per iteration,
same pattern as the X11/Wayland servers. Dispatching from the evdev
thread corrupts the main loop (node callbacks + GDScript must run on the
main thread; observed: main thread spinning at 100% after an ESC press).
- Each button emits **both** an `InputEventKey` and an
`InputEventJoypadButton`, so `Input.is_key_pressed`, InputMap key
actions, and InputMap joypad actions all work.

Mapping (SDL/Xbox convention — the same one RetroArch & co. get from
SDL on this device):

| evdev | Godot key | joypad button |
|---|---|---|
| BTN_SOUTH 304 | ENTER | A |
| BTN_EAST 305 | ESC | B |
| BTN_WEST 308 | SPACE | X |
| BTN_NORTH 307 | E | Y |
| BTN_START 315 | F1 | START |
| BTN_SELECT 314 | F2 | BACK |
| BTN_TL/TR 310/311 | Q/W | L1/R1 |
| ABS_HAT0X/Y 16/17 | arrows | DPAD |
| ABS_X/Y 0/1 | arrows | — |

**Label caveat:** the Brick's physical silkscreen is swapped relative to
the Xbox positions — south is printed **B**, east **A**, west **Y**,
north **X**. So on the Brick the *labeled A* button (east) is the one
that sends ESC/quit. This matches what SDL apps on the device do.

**Quit paths** (so the frontend always gets the display back):
1. App-level: ESC (labeled-A button) in the app's `_unhandled_input`.
2. DS-level fallback: START+SELECT held together → `SceneTree::quit()`
   from `process_events()` (flag set by the evdev thread, acted on main
   thread).

Diagnostics: `GODOT_FBDEV_EVLOG=1` → raw events to
`/tmp/godot_evdev.log`; `[evdev]` lines in `/tmp/godot_fbdev_diag.log`.

**GDScript gotcha (Godot 4.6 line):** the GDScript 2.0 tokenizer does
not support `//` line comments (only `#`) — a `//` line is a parse
error (`Expected statement, found "/"`), silently killing the whole
script at load. Verified on the 4.6.2 editor and this 4.6-stable
engine. Use `#` comments in all project scripts.

Verified on-device (2026-09-29) two ways:
- **Physical controller** (evdump capture of the real uinput device):
  A=305 (EAST), B=304 (SOUTH), X=307 (NORTH), Y=308 (WEST), L1=310,
  R1=311, Select=314, Start=315, D-pad=HAT0X/Y — all covered by the
  table above.
- **Synthetic uinput controllers** (`engine_build/tools/evinject.c`,
  `evinj_b.c`, `evinj_ss.c` — create a device with the same BTN codes
  and inject scripted presses; **always run with ES suspended**, or ES
  consumes the injected presses and launches games; injectors take an
  optional startup delay so presses land after the app's device scan):
  full button stream read, KEY+JOY echo in the test app, ESC → clean
  self-exit via the app's `_unhandled_input`, START+SELECT → clean
  self-exit via the DS fallback.

**The A133's `/userdata` (exfat) wedges minutes after boot** (reads
hang forever, silent, survives soft reboot; cold power cycle usually
recovers). The port must not read `/userdata` at launch: stage the app
to tmpfs while the volume is healthy and let the ES entry script
(`engine_build/port/godot-libpd.sh`) exec the staged copy:

```sh
mkdir -p /tmp/godot-app
cp /userdata/roms/ports/godot-libpd/{test,test.pck,libgodot_libpd.so} /tmp/godot-app/
chmod +x /tmp/godot-app/test
```

One-shot adb variant: `adb shell 'cp /userdata/roms/ports/godot-libpd/test* /userdata/roms/ports/godot-libpd/libgodot_libpd.so /tmp/godot-app/'`.
Note: background processes via adb die when the adb session closes
(SIGHUP, and `nohup` does not help) — keep the shell alive with a
trailing `sleep`. Note: an `/etc/init.d/S99*` staging script was tried and abandoned —
it vanished after a reboot (why is unexplained), so manual staging is
the working procedure.

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

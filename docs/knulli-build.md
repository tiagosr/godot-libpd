# Knulli (Allwinner handhelds) — build, display & input

This page documents the bare-Linux handheld port (validated on the
**Trimui Brick**, Allwinner A133 / Knulli 1.0.2). For day-to-day build
commands and the full firmware-caveat list see
[`engine_build/README.md`](../engine_build/README.md) — this is the
architecture and gotchas reference.

## What ships

- Custom **Godot 4.6** `template_release` engine for `linux.arm64`,
  built from the `4.6-stable` line in Docker (Ubuntu 22.04 aarch64,
  native compile): `scons platform=linux target=template_release
  arch=arm64 fbdev=yes sdl=no`.
- `godot-libpd` GDExtension (linux arm64) built with the same toolchain.
- Exported via the official 4.6.2 editor; the custom engine binary is
  installed as the `linux_release.arm64` export template (official file
  backed up first — see `package-knulli.sh`).
- Both need **glibc >= 2.35** (Knulli has 2.40).

## Display: `DisplayServerFbdev` (`platform/linuxbsd/fbdev/`)

Knulli ships no X11/Wayland. The panel is scanned out by the vendor
**PowerVR display controller** (`pvrsrvkm`), *not* by `/dev/fb0`
(`/dev/fb0` is a separate framebuffer that ES also paints; opening it
read-only is only used to get the 1024x768 geometry).

A process claims the real panel by creating an **EGL window surface** —
the vendor NULL-window-system backend (`libpvrNULL_WSEGL`,
`WSEGL_CreateWindowDrawable`, logs `MALI_CreateWindow`) allocates the
DC buffer — and presenting with **`eglSwapBuffers()`**. The fbdev
display server implements exactly that:

- One fixed fullscreen window; `eglCreateWindowSurface(display, cfg,
  /*window=*/0, NULL)` is the claim call. A PBuffer surface does *not*
  claim the display (verified on-device: renders into a buffer nobody
  scans out).
- The rasterizer is **Compatibility (OpenGL ES 3.x via the GLES3
  compatibility rasterizer)** — the project must set
  `rendering_method.linuxbsd=gl_compatibility`. (Not GLES2: Godot 4
  dropped GLES2; the device driver exposes ES 3.2.)
- `swap_buffers()` presents via `eglSwapBuffers()` and paces to ~60fps
  (`GODOT_FBDEV_FPS`). Vulkan is broken on the Brick
  (`vkCreateInstance` → `VK_ERROR_INCOMPATIBLE_DRIVER`), and there is no
  DRM/KMS, so the ES/GLES path is the only one.
- Registered **first** in `OS_LinuxBSD` so it is the default driver; on
  hosts without `/dev/fb0` (desktop dev machines) creation fails and the
  engine falls back to x11/wayland, so the same source tree builds and
  runs everywhere.

Verified on-device: ES menu → port launch → Godot splash → test UI
(stable, no flicker) → engine exit → ES menu restored.

## Input: evdev in `DisplayServerFbdev`

The Brick's controller is a **uinput device** created by the
`trimui_inputd` daemon (`/dev/input/event3`, `TRIMUI Brick Controller`,
`045e:028e:0114`). The display server reads it with a dedicated
thread that scans `/dev/input/event*` at startup and `poll()`s all
open devices:

- **Events are never dispatched from the evdev thread.** They are
  queued (mutex-protected) and delivered to
  `Input::parse_input_event()` from `DisplayServer::process_events()`
  on the main thread — the same pattern as X11/Wayland. Dispatching
  from the evdev thread corrupts the main loop (observed: 100% main
  thread spin, quit keys dead).
- **SDL channel model:** physical buttons emit both an
  `InputEventKey` and an `InputEventJoypadButton`; the D-pad (hat)
  emits **joypad buttons only** (a gamepad hat is a gamepad channel —
  no arrow-key synthesis from it).
- **Logical press de-dup:** firmware input layers can mirror one
  physical button across several event devices; the evdev thread tracks
  logical pressed state per key/joypad button (hat state globally) so
  each press/release is emitted exactly once.
- **CRITICAL: build with `sdl=no`.** With the default `sdl=yes`,
  `OS_LinuxBSD::initialize_joypads()` creates a `JoypadSDL` whose
  built-in SDL evdev backend opens the *same* `/dev/input` devices and
  feeds them into `Input` a second time — every press reaches scripts
  **twice** as two distinct event objects (GUI navigation
  double-steps, actions double-fire). It is invisible in raw evdev
  logs and survives in-reader dedup; only removing the SDL joypad
  driver fixes it.
- Quit paths: app-level `ui_cancel` (ESC/Back), plus a DS-level
  START+SELECT fallback → `SceneTree::quit()`.

Physical mapping (the Brick's silkscreen is swapped vs Xbox: south is
printed **B**, east **A**, west **Y**, north **X** — matching what SDL
apps on the device see):

| evdev | Godot key | joypad button |
|---|---|---|
| BTN_SOUTH 304 | ENTER | A |
| BTN_EAST 305 | ESC | B |
| BTN_WEST 308 | SPACE | X |
| BTN_NORTH 307 | E | Y |
| BTN_START 315 | F1 | START |
| BTN_SELECT 314 | F2 | BACK |
| BTN_TL/TR 310/311 | Q/W | L1/R1 |
| ABS_HAT0X/Y 16/17 | — (joypad only) | DPAD |

## Audio

v1 uses the Godot-native `AudioStreamGenerator` sink (see the design
spec); on the Brick playback is verified through the 3.5 mm jack.
Native ALSA/PipeWire backends are v2 (the `PdAudioSink` interface
already isolates the swap).

## v1 status (all verified on the Brick)

- [x] Engine load + extension load (SMOKE_OK headless, 10/10)
- [x] Display: vendor DC via EGL window surface, stable 60fps
- [x] Input: single-delivery evdev, physical + synthetic verified
- [x] Audio: libpd patch load, DSP start, audible output
- [x] ES menu integration (tmpfs staging, exfat-safe entry script)

## Firmware caveats (A133, kernel 4.9) — the short list

- `/userdata` is **exfat-FUSE and wedges silently** minutes after boot —
  stage the app to `/tmp` (tmpfs) and run from there; see
  `engine_build/README.md` for the exact procedure.
- `/etc/init.d` additions **do not persist** across reboots.
- The shell is **dash**, not busybox; avoid nested `$(...)` in
  launcher scripts (dash mis-parses them here).
- Never kill the host adb session mid-run — orphaned background
  processes spin unkillable; keep a trailing `sleep` in test shells.
- Injection tests must run with EmulationStation **suspended**
  (`/etc/init.d/S31emulationstation suspend`), else injected Start
  presses launch games.
- Godot 4.6 GDScript has **no `//` comments** — `#` only.

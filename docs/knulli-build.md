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

### MIDI I/O (RtMidi — ALSA sequencer; verified on the A133)

v2: MIDI I/O (RtMidi ALSA backend on A133 — `aconnect` loopback
verified on-device 2026-10-02 with the M3 `RtMidiHostBackend`). The
M1/M2 PortMIDI backend was retired from the project 2026-10 (the
upstream patchset remains at `patches/portmidi-upstream/`).

The A133's ALSA sequencer exposes **no SUBS-capable ports**
(`aconnect -l` shows nothing else, `getPortCount()` == 0 for the
app), so the `test_midi` scene creates the app's own snd_seq virtual
ports — `Libpd.server.midi_open_virtual_input()` /
`midi_open_virtual_output()` (named `"libpd test app in 0"` /
`"libpd test app out 0"`) — and loopback is wired with `aconnect`
between the app's own ports:

1. Stage + launch `test_midi` (tmpfs staging per
   `engine_build/README.md`; ES suspended). The Brick has no
   touchscreen — drive the scene's buttons with the D-pad focus
   (down/right + Enter) or the uinput key-injection helper (`uikeys`;
   extended source kept on the device at `/tmp/uikeys.c`).
2. In the app: **Open Virtual In**, then **Open Virtual Out**. RtMidi
   registers the two ports on **two separate snd_seq clients** (both
   named `godot-libpd`) — unlike the old PortMIDI single client with
   two ports. Read the client numbers from `aconnect -l` each run.
3. From a second adb shell, check and wire out → in:
   ```sh
   aconnect -l            # two 'godot-libpd' clients, one port each
   aconnect 129:0 128:0   # out client port 0 -> in client port 0 (per boot)
   ```
4. In the app: **Route In → instance**, **Route instance → Out**, then
   **Send test note** → the looped note appears in the event log and
   on stdout: `[MIDI] note_on port=0 ch=0 pitch=60 vel=100`.
5. Negative test: `aconnect -d 129:0 128:0`, send again → the note is
   written to the output port but nothing is wired back, so **no new
   `note_on`** appears.

On-device stdout gotchas (both verified):

- **Launch line-buffered** — redirecting the app's stdout to a file
  makes it block-buffered and hides every `[MIDI]`/`[UI]` line until
  4 KB accumulates. Run
  `stdbuf -oL -eL ./test ... > /tmp/app.log 2>&1`.
- **The engine loads `test.pck` by basename** — the knulli engine binary
  is named `test`, so it loads `test.pck` next to it and **ignores the
  positional PCK argument**. Stage the scene you want as `test.pck`.
- **`uikeys` must exist before the app starts** — the app's evdev thread
  scans `/dev/input` at startup only.

Caveats:

- **The loopback patch must have `[noteout]`.** `smoke_patch.pd`
  (notein → print + noteout) is loaded by both `test_midi` modes; the
  audio demo `test_patch.pd` has no `[noteout]`, so with it the app
  output is empty and nothing can loop (this was the first on-device
  "loopback broken" repro — delivery was fine all along).
- **Echo loopback is a feedback loop.** `smoke_patch.pd` wires
  notein → noteout, so while `aconnect` is wired, every sent note
  feeds back indefinitely (observed on-device: sustained note_on
  re-emission, hundreds of thousands of events per minute). Unwire
  (`aconnect -d`) as soon as verification is done.
- **Timestamp-queue guard (upstream PortMIDI patchset — historical
  context).** The upstream PortMIDI ALSA backend stamped each port with a
  shared seq queue that is lazy-allocated at the first `Pm_Open`; ports
  created before that carry queue 0, which on the A133 is owned by another
  client (PipeWire) and Stopped — the kernel never delivers
tick-timestamped events on a queue the port's client does not own. The
  fix (create ports with kernel-default real-time timestamping unless the
  shared queue already exists) lives in `patches/portmidi-upstream/` for
  the eventual upstream PR. It does not affect this project's RtMidi
  backend, which creates the ports directly and does not use that
  shared-queue path. `aconnect -l`
  / `/proc/asound/seq/queues` show the queue ownership;
  `/proc/asound/seq/ports` does not exist on this kernel (4.9), so
  per-port timestamp flags are not directly observable.
- **Zero devices is normal here.** Opening an index while the device
  list is empty fails cleanly (a `midi_port_error` signal, no freeze) —
  router-side index validation plus a backend-side bounds check (the
  RtMidi backend validates the index against its own enumeration and
  returns `InvalidParameter`; the PortMIDI build's equivalent was the
  vendored `Pm_OpenInput` check — now upstream patchset only). `Pm_CountDevices()` == 0 made index 0 an
  out-of-bounds descriptor read on the I/O thread — the original A133
  freeze.

**Hotplug (M4) — live device add/remove.** The router re-enumerates
`list_ports()` every `midi_port_poll_interval` seconds (default 0.5) and
fires `midi_port_added` / `midi_port_removed` on the main thread, keyed by
**(direction, name) — not index** (indices shift when the device set
changes). Removal is debounced (**two consecutive empty enumerations**) so a
transient bad ALSA probe tick doesn't flap ports; open real-device ports
whose device disappeared are auto-closed with
`midi_port_error(port, "device removed")`. No auto-reopen (the app re-opens
by re-querying `list_ports()`).

On the A133 the device-free vehicle is the app's own virtual ports (the
backend creates real snd_seq clients visible to `aconnect`):
`midi_open_virtual_input()` makes the port appear → `midi_port_added`;
`midi_close_input()` makes it vanish → `midi_port_removed`. The ALSA name is
the full `"godot-libpd:<port> <client>:<port>"` string (e.g.
`"godot-libpd:libpd test app in 0 128:0"`), unlike CoreMIDI's bare name —
match on a substring.

**Real USB-MIDI hotplug on the A133 (OTG):** the A133 has a **host-only OTG
port** (separate from the adb USB-C port), so a physical USB-MIDI device
plugged there appears as an ALSA kernel client (`aconnect -l` →
`client NN '<dev>' [type=kernel,card=1]`, card=1 = USB). Use
`--midi-hotplug-monitor` (keeps the app running, logs every live
add/remove/error, auto-opens new input ports so an unplug exercises
auto-close). Because the app must run **foreground** on the A133, use
**concurrent adb sessions**: launch the monitor in a backgrounded adb session
(`adb shell "... ./test --midi-hotplug-monitor > /tmp/monitor.log 2>&1" &`)
and read `/tmp/monitor.log` from a second session. Verified with a
nanoKONTROL2 and an FM-1 (three plug/unplug cycles): each plug →
`midi_port_added` for both sides + auto-open (RtMidi ALSA then creates a
`godot-libpd:<dev> 128:0` receive port, seen as an extra output); each
unplug → `midi_port_error(port, "device removed")` (auto-close) +
`midi_port_removed` for the device and its receive port.

Device-free self-test: stage `test.pck` (main scene `test_midi.tscn`) + the
`.so`, then `stdbuf -oL -eL ./test --midi-hotplug-smoke` →
`HOTPLUG_SMOKE_OK added=1 removed=1`. **Run it foreground in the same adb
command** — a backgrounded app on the A133 stalls at init (fbdev/vsync +
SIGHUP on adb close); the `<basename>.pck` + `uikeys` + `stdbuf` gotchas
above apply.

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

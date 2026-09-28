# Godot libpd GDExtension — Design (v1)

Date: 2026-09-27
Status: Approved for implementation planning

## 1. Objective

Produce a working, multi-platform **libpd GDExtension** for Godot 4.6, including:

- **Multi-instance support**: any number of independent Pure Data instances, each on its own worker thread.
- **Worker-thread DSP**: `libpd_dsp()` runs on a dedicated per-instance thread; the main thread can freely do UI/patch work.
- **Engine build for Knulli/Batocera handhelds** (Allwinner A133 — Trimui Brick — and H700 — RG35XX*/RGCubeXX*) as a linux-arm64 release binary, built via Docker on Apple Silicon (linux/arm64).
- **Simple test app** proving audio + multi-instance + print output on:
  1. macOS (Apple Silicon, release export, Godot 4.6.2 editor)
  2. Android (arm64-v8a, debug sideload)
  3. Trimui Brick (A133, Knulli CFW)

## 2. Version pins

| Component | Version |
|---|---|
| Godot engine (Knulli build) | tag `4.6-stable` |
| Godot editor (user's, for exports) | 4.6.2 |
| macOS + Android export templates | **official 4.6.2 templates** (user supplies; no template builds needed) |
| godot-cpp | 4.5 stable (latest godot-cpp release; GDExtension 4.x ABI is forward-compatible with the 4.6.2 engine), pinned commit |
| libpd | submodule `modules/libpd` @ `ba0dc63` (v0.16.1, pd vanilla 0.56-5) |
| PortMIDI | submodule `modules/portmidi` @ `6b51c25` (compiled in v1, **not wired up** — v2) |
| Docker base (Knulli) | `linux/arm64 ubuntu:22.04` (pattern from `../trackerjolo-v/Dockerfile.knulli`) |
| Android NDK | r26 (in NDK Docker image) |

## 3. Chosen approach

Standard **godot-cpp (CMake) GDExtension** with a thin GDScript-friendly
**multi-instance Node API**:

- One C++ GDExtension package; libpd (and pd) compiled in as static libraries
  with `-DPD_MULTI=ON` (`PDINSTANCE`+`PDTHREADS`) for multi-instance.
- Each `LibpdInstance` Node owns one pd instance + one worker thread + one Godot-native audio sink.
- Audio v1 (**a1**): output blocks pushed into an `AudioStreamGenerator` owned by the Node; Godot's audio server does playback. No native audio backends in v1.
- v2 register (explicit):
  - native audio backends (**a2**): CoreAudio / OpenSL ES / ALSA, behind the sink interface
  - MIDI I/O via PortMIDI (already compiled in)
  - audio input / microphone capture

## 4. Public API

### `LibpdServer` (singleton Node)

Central event hub. Worker threads push plain C structs into a lock-free ring; the server drains it in `_process()` (main thread only) and forwards events as Godot signals.

```gdscript
LibpdServer.instance_print(id: int, text: String)      # libpd printhook
LibpdServer.instance_note_on(id: int, ch: int, pitch: int, vel: int)  # noteonhook
LibpdServer.instance_dsp_active(id: int, active: bool) # worker start/stop/failure
LibpdServer.get_instance(id: int) -> LibpdInstance
```

No worker thread ever calls into Godot or emits signals directly. This is the
only thread-crossing mechanism → deadlock-free by construction.

**Drain strategy (Task 6 finding):** the ring is drained one event at a time
(`pop(&e, 1)`, emit, repeat), bounded to 128 events/frame. A batch `pop` into a
large stack buffer (`PdEvent events[128]`) followed by a tight `emit_signal`
loop dropped signal deliveries under load (reproducible: 4 workers, ~1 of ~4
prints reached the main thread). One-at-a-time pop+emit is correct, keeps each
emit independent, avoids the large stack allocation, and is still bounded per
frame so a print flood cannot starve the main thread.

### `LibpdInstance` (Node)

```gdscript
# lifecycle
# blocksize is fixed at pd compile time (libpd_blocksize()); not a parameter
init(samplerate: int = 44100, n_ins: int = 0, n_out: int = 2) -> bool
load_patch(path: String, search_paths: PackedStringArray = []) -> Error
unload_patch()                      # auto-stops dsp if active
start_dsp()                        # spawns worker thread
stop_dsp()                         # joins worker thread

# patch control — all calls are queued to the instance's worker thread;
# safe to call from the main thread, executed between dsp blocks
send_pd_message(receiver: String, args: PackedStringArray)
set_parameter(path: String, value: float)
send_midi(channel: int, pitch: int, velocity: int)

# state
patch_loaded: bool
dsp_active: bool
samplerate: int
```

Signals: `ready`, `failure(code: Error, text: String)`.

## 5. Threading model

- **Per-instance worker thread** runs, in order: (1) drain the command queue
  (init/load/message/midi/unload commands pushed by the main thread),
  (2) `libpd_process_float(1, nullptr, outbuf)` (libpd 0.16.1 has no
  `libpd_dsp()`; the dsp entry is the `process_*` family), (3) push output
  into the audio sink, (4) pace to `libpd_blocksize() / samplerate` cadence
  via time-based sleep.
- **All libpd/pd calls for an instance happen only on its worker thread.**
  Main-thread API pushes commands into a per-instance thread-safe queue
  (libpd 0.16 ships `z_queued.c` for this). No locking of pd state anywhere.
- **Teardown order** (main thread only): signal stop-atomic → join worker →
  destroy pd instance (`libpd_free_instance`).
- Printhook/noteonhook (called from the worker thread, and possibly from dsp
  context) copy into the `LibpdServer` event ring with instance id attached
  (via `libpd_set_instancedata`).

## 6. Audio (v1, a1)

- Worker produces `libpd_blocksize()` sample blocks at the instance
  samplerate (default 44100) and pushes them into an `AudioStreamGenerator`
  (buffer 2048 samples, chosen so buffer length is a multiple of blocksize)
  driven by an `AudioStreamPlayer` per `LibpdInstance` node. The generator
  is driven from the worker thread via `play_from_thread()`.
- Output-only in v1: `n_ins = 0`.
- **Fail-fast**: if `AudioServer.get_mix_rate()` != instance samplerate at
  `init()`, return an error with a clear message instead of producing
  garbage audio. (Project `AudioServer` mix rate must be set to 44100 in the
  test app.)
- Pacing is time-based; Godot's audio server owns playback timing.
  Expect ~10–20 ms extra latency vs. native backends — acceptable for v1.
- The sink sits behind a small C++ interface (`PdAudioSink`) so v2 can swap
  in CoreAudio/OpenSL/ALSA implementations per platform without touching the
  worker or the API.

## 7. Repo layout

```
godot-libpd/
├── (Godot engine source @ tag 4.6-stable; unchanged apart from submodules)
├── modules/libpd, modules/portmidi        (submodules — reference for versions)
├── extension/
│   ├── CMakeLists.txt                     # godot-cpp + libpd + portmidi + wrapper
│   ├── thirdparty/
│   │   ├── godot-cpp/                     (pinned 4.6)
│   │   ├── libpd/                         (submodule @ ba0dc63, incl. pure-data)
│   │   └── portmidi/                      (submodule @ 6b51c25)
│   ├── src/
│   │   ├── libpd_server.{h,cpp}           # singleton, event ring, main-thread drain
│   │   ├── libpd_instance.{h,cpp}         # Node, pd instance, command queue
│   │   ├── libpd_worker.{h,cpp}           # worker thread: queue drain + dsp loop
│   │   ├── pd_audio_sink.{h,cpp}          # v1: AudioStreamGenerator sink
│   │   └── register_types.{h,cpp}
│   ├── godot-libpd.gdextension            # per-platform entry points
│   └── build.sh                           # --macos | --linux-arm64 | --android
├── engine_build/
│   ├── Dockerfile.knulli                  # ubuntu 22.04 arm64, aarch64 gcc
│   └── build-knulli-engine.sh             # BOARD=a133|h700 → scons release arm64
├── test_project/
│   ├── project.godot                      # Godot 4.6, AudioServer rate 44100
│   ├── scenes/test.tscn
│   ├── scripts/test_main.gd
│   └── data/test_patch.pd                 # osc~ + sig~ from libpd samples
├── dist/                                  # build outputs (gitignored)
│   └── knulli-<board>/                    # release binary + exported test app + godot.sh
└── docs/superpowers/specs/…               # this document
```

## 8. Build matrix

| Artifact | Method |
|---|---|
| GDExtension macOS arm64 (`.dylib`) | native CMake, `build.sh --macos` |
| GDExtension linux arm64 | `build.sh --linux-arm64` inside the Knulli Docker image (aarch64 gcc) |
| GDExtension Android arm64-v8a (`.so`) | `build.sh --android` inside NDK r26 Docker image (linux/arm64) |
| Godot 4.6 linux-arm64 **release** engine (Knulli) | Docker scons `platform=linux target=release arch=arm64`, native aarch64 |
| macOS / Android export templates | **official 4.6.2** (user-supplied; not built) |

Docker runs natively as `linux/arm64` on the Apple Silicon host (same
pattern as `../trackerjolo-v/Dockerfile.knulli`). `BOARD=a133|h700` affects
packaging only, not the build (image is board-agnostic).

**Knulli packaging** mirrors the trackerjolo flow: `dist/knulli-<board>/`
contains the release binary + the exported `test_project` + a `godot.sh`
launcher; the whole folder is copied to the device (e.g. `roms/ports/`) and
run like a Knulli `.sh` launcher.

### Known risk: old glibc/kernel on Knulli

Kernel is 4.9 (A133: 4.9.191, H700: 4.9.170) and the firmware glibc may be
older than Godot 4.6's linux-arm64 expectations. Detection happens on first
device test. Contained fallbacks, no other part of the design changes:

1. build the engine with a lowered minimum glibc (`--glibc-version`), or
2. statically link libc into the release binary in the Docker build.

### Export & extension build gotchas (Task 7 findings)

All discovered while making the macOS release export pass `--smoke` (editor
headless runs passed throughout — several of these only manifest in the
**release template**):

1. **godot-cpp must be generated with `GODOTCPP_TARGET=template_release`.**
   The default `template_debug` target compiles godot-cpp with
   `DEBUG_ENABLED`. A `DEBUG_ENABLED` dylib loaded into a *release* export
   template (built without `DEBUG_ENABLED`) corrupts the heap during class
   registration (signal/method binding) → SIGSEGV/SIGABRT, 100%
   reproducible, at any point in shutdown/init. The debug template and the
   editor (both `DEBUG_ENABLED`) load the debug-target dylib fine, which is
   what hid it. The `template_release` dylib loads correctly in the editor,
   debug template, and release template alike — one build serves all.
   (`build.sh` sets the flag; do not remove it.)
2. **macOS arm64 needs a custom template.** The official 4.6.2 `macos.zip`
   ships only `godot_macos_{debug,release}.universal`; the exporter demands
   `godot_macos_{debug,release}.arm64` for `binary_format/architecture=
   "arm64"`. `make-macos-arm64-template.sh` (repo root) lipos the arm64
   slices and repackages → `test_project/export_templates/macos-arm64.zip`
   (gitignored artifact; referenced by the preset's `custom_template`).
3. **`rendering/textures/vram_compression/import_etc2_astc=true`** must be
   enabled in `project.godot` or the macOS arm64 export is refused.
4. **Text data files are not exported by `all_resources`.** The exporter
   skips files the editor classifies as `TextFile`; `test_patch.pd` would
   silently be absent from the pck. The presets set `include_filter="*.pd"`
   to force-include it. (Same will apply to any future text data files.)
5. **libpd opens real files, not pck resources.** In exported builds
   `globalize_path("res://…")` returns a path (often relative) that Godot's
   pck-aware `FileAccess` can read but libpd's C `open()` cannot. `load_patch`
   therefore only trusts a globalized path that is absolute *and* exists on
   the real filesystem; otherwise it extracts the resource to
   `<user_dir>/godot_libpd/<file>` first.
6. **`--smoke` mode**: the test app runs a headless 6-step self-test when
   launched with `--smoke` (init → load → dsp+blocks → note→print →
   2nd instance spawn/kill → teardown) and exits 0/1. This is the automated
   acceptance check for every platform's exported app.

## 9. Test app

Single scene, `test.tscn`, one `Control` root with:

| UI | Behavior |
|---|---|
| Label | title, instance count, samplerate |
| "Load Patch" | `instance.load_patch("res://data/test_patch.pd")` |
| "Start/Stop DSP" | toggles `start_dsp()`/`stop_dsp()` on all instances |
| "Send Test Note" | `send_midi(0, 60, 100)` on all instances (proves main→worker queue) |
| "+Instance" | adds another `LibpdInstance` (multi-instance proof) |
| "Kill Last" | frees the last instance (clean teardown proof) |
| RichTextLabel | last 20 lines of `instance_print`, auto-scroll |

`test_patch.pd`: `[osc~ 440] → [sig~] (gated by the test note) → [out~]`,
based on `libpd/samples`.

**Success criteria (all platforms):**
load → start → audible sine → print lines visible → add/kill a 2nd instance
without crash → stop. On the Trimui Brick: folder copied over USB, launched
via `./godot.sh`, audio out of the 3.5 mm jack.

## 10. Error handling

- Every fallible C++ call maps to a Godot `Error` code + `failure(code, text)`
  signal. No asserts/crashes on bad user paths:
  - double `init()` → error, state unchanged
  - `load_patch` before `init` → error
  - `unload_patch` while dsp active → auto-stop first
- Worker threads always check a stop-atomic at each block boundary.
- Worker uncaught exceptions → log + mark instance failed, never kill the process.
- `AudioServer` rate mismatch → fail fast at `init()` (see §6).

## 11. Out of scope (v1)

- MIDI I/O via PortMIDI (v2; library already compiled in)
- audio input / microphone capture (v2)
- native audio backends a2: CoreAudio / OpenSL ES / ALSA (v2, sink interface exists)
- iOS, Windows, Web
- sample-accurate latency guarantees
- pd patches requiring external objects
- building official-platform export templates (official 4.6.2 used)

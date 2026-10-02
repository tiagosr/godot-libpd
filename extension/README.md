# godot-libpd GDExtension

Multi-instance, worker-threaded libpd (Pure Data) GDExtension for Godot 4.6,
with Godot-native audio (AudioStreamGenerator). See
`../docs/superpowers/specs/2026-09-27-godot-libpd-gdextension-design.md`.

## Pinned third-party versions

| Submodule | Pin |
|---|---|
| `thirdparty/godot-cpp` | branch `4.5`, commit `27d9dd2` (godot-4.5-stable-30-g27d9dd2) |
| `thirdparty/libpd` | `ba0dc63` (libpd 0.16.1, pd vanilla 0.56-5; pure-data submodule @ f009fd8) |
| `thirdparty/portmidi` | `6be63b7` (v2.0.8-4-g6be63b7; compiled in on macOS/Linux, see `../patches/portmidi-upstream/`) |
| `thirdparty/rtmidi` | `b8b2720` (upstream master `23b8cd5` + 3 local Android fixes, see below) |

## Building

```
./build.sh --macos          # extension/build/macos/libgodot_libpd.dylib
./build.sh --linux-arm64    # extension/build/linux/libgodot_libpd.so  (inside Knulli Docker)
./build.sh --android        # extension/build/android-arm64/libgodot_libpd.so (NDK in $NDK)
```

Note: CMake >= 4.x may require `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
depending on godot-cpp's scripts (tracked here if needed).

## Testing

Headless tests live in `../test_project` (main scene switched per test;
see the implementation plan). Unit tests: `ctest --test-dir build/cmake-macos`.

## Multi-instance MIDI hook attribution

Each `LibpdInstance` owns a `LibpdWorker` thread, and that thread is the
ONLY thread that ever calls into libpd for that instance: the worker
creates its own pd instance during INIT (`libpd_init` ->
`libpd_new_instance` -> `libpd_set_instance`) and executes every command
(MIDI input, load/unload, dsp) on that thread.

Output hooks fire synchronously on the thread that triggered the output
(`libpd_noteon` etc. run to completion on the caller). The hook
trampolines in `src/libpd_worker.cpp` resolve their owning worker via
`libpd_get_instancedata()`, which under a multi-instance build resolves
per instance: libpd's `LIBPDSTUFF` is `STUFF->st_impdata` (per instance)
whenever `PDINSTANCE` is defined, and `pd_this` — the current instance —
is thread-local (`__thread` in `pure-data/src/m_pd.h`). So every hook
event lands in the emitting instance's own worker `midi_out` queue, and
routing fan-out (A -> MIDI port -> B) delivers each note to exactly the
instances routed from that port.

This REQUIRES the multi-instance build: `PD_MULTI` is forced ON in
`extension/CMakeLists.txt` (see the guardrail comment there). With
`PD_MULTI OFF`, `LIBPDSTUFF` falls back to the single process-wide
`libpd_mainimp`, and every instance's hooks/instancedata collapse into
one — notes from any instance would be attributed to the worker that
last called `libpd_set_instancedata`. Do not turn `PD_MULTI` off.

> Review note (2026-09-30 whole-branch review, P1): the review's concern
> that a single process-wide `i_data` misroutes multi-instance hooks was
> investigated and disproven for this build — `PD_MULTI ON` is forced,
> so the per-instance path above is the one that compiles. The
> regression tests in `tests/multi_instance_midi_tests.cpp`
> (`multi_instance_midi_tests` ctest target) lock in correct attribution:
> two real workers each drive their own instance, a note through one
> appears only in that worker's queue, and a routed A -> port -> B
> fan-out reaches only B.

## MIDI backends (v2 M1 + M2)

MIDI I/O is owned **server-wide** by `LibpdServer` (not per instance) and
runs on a single dedicated **MIDI I/O thread** (`MidiRouter`). All `libpd_*`
calls stay on each instance's worker thread; all device writes
(`PmWrite*` / RtMidi `sendMessage`) happen on the MIDI I/O thread; input
callbacks only copy bytes into per-port rings and are drained on the I/O
thread. Godot-facing events cross to the main thread as typed signals.

The concrete backend is selected at build time by `src/midi_backend_factory.cpp`:

| Platform | Backend | Source |
|---|---|---|
| macOS / Linux | `PortMidiBackend` | vendored PortMIDI (`thirdparty/portmidi`) |
| Android | `RtMidiAndroidBackend` | vendored RtMidi, `ANDROID_AMIDI` API |

Both implement the same godot-free `MidiBackend` interface
(`src/midi_backend.h`), so the router and the GDScript API are identical
across platforms. `tests/midi_backend_fake_tests.cpp` pins the interface
contract (dual note+CC delivery, full-sysex command, raw framing,
virtual-loopback round-trip).

### Delivery model (option C)

Every incoming MIDI event is delivered **dual**: as a high-level libpd
message (`libpd_noteon` -> `[notein]`, `libpd_cc` -> `[ctrl]`, ...) **and**
as raw bytes to the instance's `[midiin]` (`libpd_midibyte`). Output is
captured from the pd hooks and fanned out to every output port the
instance is routed to.

### Sysex

**Input-only.** Sysex arrives whole-message (the read stage reassembles
`F0..F7` runs) and is surfaced as the typed `midi_sysex(port, data)`
signal; there is **no** `[sysexout]`/sysex send in this libpd build, so the
backend exposes a `write_sysex()` only for completeness (PortMidi hosts
write it to the port; the GDScript API does not expose sysex send). A
127-data-byte cap applies per message.

### Android specifics (M2)

- **minSdk 29** — the NDK **AMidi** C API is API 29+ (the Java MIDI API is
  26, but we use the native one). `build.sh --android` targets
  `android-29`. The backend re-checks `Build.VERSION.SDK_INT >= 29` at
  runtime and degrades to `midi_available() == false` below it.
- **In-process virtual loopback** — Android has no virtual MIDI device API
  and RtMidi's `openVirtualPort` is not implemented there, so the backend
  implements a loopback pair (device indices **200** in / **201** out)
  backed by a `WordRing`. `LibpdServer.midi_create_loopback(name)`
  activates it; it then appears in `midi_list_inputs()`/`midi_list_outputs()`
  as `"<name> in"` / `"<name> out"` and is opened with the ordinary
  `midi_open_input()`/`midi_open_output()`. On PortMidi hosts this is
  unavailable (use IAC on macOS / `aconnect` on Linux).
- **One RtMidi object per open port** — RtMidi's Android backend supports
  exactly one open port per `RtMidiIn`/`RtMidiOut`; the backend keeps a
  `unique_ptr` per handle so the input-callback pointer stays stable.
- **Async open** — `MidiManager.openDevice` is async with **no failure
  callback**, so an open is followed by a bounded settle-wait (300 ms) on
  the I/O thread; a failed open surfaces as silent non-delivery (documented
  platform limitation).
- **`JavaVM*` capture** — an Android app's linker namespace blocks every
  `dlopen` of `libart` and the VM is not in `RTLD_DEFAULT` scope, and a
  GDExtension `.so` never gets `JNI_OnLoad`. The backend resolves
  `JNI_GetCreatedJavaVMs` by parsing libart's ELF dynamic symbol table via
  `/proc/self/maps` + `/proc/self/mem` (guard refuses to call a pointer
  outside an `r-x` libart mapping). RtMidi's own `androidGetThreadEnv` reuses
  the result via the `gdpd_rtmidi_host_java_vm()` host hook. Full details:
  `../docs/android-build.md` → "How the extension gets a `JavaVM*`".
- **Local RtMidi fixes** (submodule `b8b2720`, 3 commits on upstream
  `23b8cd5`, to be folded into a future upstream PR alongside the PortMIDI
  patchset): multi-chunk sysex accumulation across `pollMidi` iterations
  (`759d4e6`), and Android `JavaVM` resolution via the host resolver
  (`9727ab6` superseded by `b8b2720`).

### On-device verification status (M2)

Verified on an Android 14 arm64 device (Anbernic RK3568; the plan targeted
the Retroid RG DS — same OS-level constraints): `MIDI_SMOKE_OK
print=1 note=1 fanout=1` over the in-process loopback, including a
second-instance (A -> loopback -> B) fan-out with per-instance print
attribution; clean exit, zero crashes. **Sysex input and CC capture are not
exercisable on a device with no MIDI hardware** (input-only sysex by
design; CC needs a real controller) — the sysex reassembly logic is covered
by the host read-stage tests and the CC path by the macOS IAC + MIDI-Learn
flow (M1). Real-hardware I/O is a deferred milestone.

See `../docs/superpowers/specs/2026-10-01-godot-libpd-android-midi-design.md`
(status: implemented) and `../docs/superpowers/plans/2026-10-01-godot-libpd-android-midi.md`.

# godot-libpd GDExtension

Multi-instance, worker-threaded libpd (Pure Data) GDExtension for Godot 4.6,
with native multi-instance audio (mix-down backend: PortAudio on
macOS/Linux, OpenSL ES on Android). See
`../docs/superpowers/specs/2026-09-27-godot-libpd-gdextension-design.md`.

## Pinned third-party versions

| Submodule | Pin |
|---|---|
| `thirdparty/godot-cpp` | branch `4.5`, commit `27d9dd2` (godot-4.5-stable-30-g27d9dd2) |
| `thirdparty/libpd` | `ba0dc63` (libpd 0.16.1, pd vanilla 0.56-5; pure-data submodule @ f009fd8) |
| `thirdparty/rtmidi` | `748eb75` (upstream master `23b8cd5` + 4 local fixes: Android sysex + JavaVM host hook + hotplug-test guard, see below) |

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

## MIDI backends (v2 M1–M4)

MIDI I/O is owned **server-wide** by `LibpdServer` (not per instance) and
runs on a single dedicated **MIDI I/O thread** (`MidiRouter`). All `libpd_*`
calls stay on each instance's worker thread; all device writes
(RtMidi `sendMessage` / `PmWrite*`) happen on the MIDI I/O thread; input
callbacks only copy bytes into per-port rings and are drained on the I/O
thread. Godot-facing events cross to the main thread as typed signals.

The concrete backend is selected at build time by `src/midi_backend_factory.cpp`:

| Platform | Backend (default) | Source |
|---|---|---|
| macOS | `RtMidiHostBackend` (CoreMIDI) | vendored RtMidi (`thirdparty/rtmidi`) |
| Linux | `RtMidiHostBackend` (ALSA) | vendored RtMidi (`thirdparty/rtmidi`) |
| Android | `RtMidiAndroidBackend` | vendored RtMidi, `ANDROID_AMIDI` API |

RtMidi is the **only** MIDI backend on **all** platforms (M3; the M1/M2
PortMIDI fallback was retired from the project 2026-10 — the upstream
patchset remains at `../patches/portmidi-upstream/`). The router logs
`[MIDI] backend=<name>` once at startup so a smoke run shows which backend
is active.

Both RtMidi backends implement the same godot-free
`MidiBackend` interface (`src/midi_backend.h`), so the router and the
GDScript API are identical across platforms. `tests/midi_backend_fake_tests.cpp`
pins the interface contract (dual note+CC delivery, full-sysex command,
raw framing, virtual-loopback round-trip) and
`tests/midi_rtmidi_host_backend_tests.cpp` covers the host backend's
device-free surface (in-process loopback round-trip, sysex shape guards).

### Delivery model (option C)

Every incoming MIDI event is delivered **dual**: as a high-level libpd
message (`libpd_noteon` -> `[notein]`, `libpd_cc` -> `[ctrl]`, ...) **and**
as raw bytes to the instance's `[midiin]` (`libpd_midibyte`). Output is
captured from the pd hooks and fanned out to every output port the
instance is routed to.

### Sysex

**Input-only.** Sysex arrives whole-message — RtMidi reassembles `F0..F7`
runs on every platform (recon-verified: a full 7-byte `F0..F7` round-trips
on macOS IAC and on the A133 ALSA loopback) — and the read stage re-slices
it into the `<=4`-byte words the raw path expects. It is surfaced as the
typed `midi_sysex(port, data)` signal; there is **no** `[sysexout]`/sysex
send in this libpd build, so the backend exposes a `write_sysex()` only for
completeness (it writes the whole `F0..F7` to the port; the GDScript API
does not expose sysex send). A 127-data-byte cap applies per message.

### Host specifics (M3 — CoreMIDI / ALSA)

- **Virtual ports are real.** `create_virtual_input/output` reserve an
  internal index (300+) and `open` calls RtMidi's `openVirtualPort()`
  (verified both directions on macOS and the A133 kernel 4.9). This is the
  A133 loopback path: the app creates its own in+out ports, then an
  external `aconnect` wires them.
- **ALSA separate clients** — unlike PortMIDI (one client, two ports),
  RtMidi registers the input and output virtual ports on **two separate**
  snd_seq clients (both named `godot-libpd`). Read the actual client
  numbers from `aconnect -l` each run and wire
  `aconnect <out-client>:0 <in-client>:0`.
- **Synchronous open** — `openPort()`/`openVirtualPort()` return with the
  port live (no Android settle-wait).
- **In-process loopback** — the host backend also implements the
  device-free loopback pair (indices **200** in / **201** out, `WordRing`
  backed) used by the host unit test; macOS uses IAC and the A133 uses
  `aconnect` for real loopback.

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
  `midi_open_input()`/`midi_open_output()`.
- **One RtMidi object per open port** — RtMidi supports exactly one open
  port per `RtMidiIn`/`RtMidiOut` on every API; every RtMidi backend keeps
  a `unique_ptr` per handle so the input-callback pointer stays stable.
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
- **Local RtMidi fixes** (submodule `748eb75`, 4 commits on upstream
  `23b8cd5`, to be folded into a future upstream PR alongside the PortMIDI
  patchset): multi-chunk sysex accumulation across `pollMidi` iterations
  (`759d4e6`), Android `JavaVM` resolution via the host resolver
  (`9727ab6` superseded by `b8b2720`), and guarding the ALSA hotplug
  regression test behind `RTMIDI_BUILD_HOTPLUG_TEST` so embedders can skip
  its build-time check (`748eb75`).

### USB-MIDI hotplugging (M4)

Live device add/remove. The router periodically re-enumerates the backend's
`list_ports()` (default cadence 0.5 s) and diffs the set, firing
`midi_port_added` / `midi_port_removed` on the main thread for changes.
This is the **only** mechanism — no backend (CoreMIDI / ALSA / AMIDI)
exposes a public "port changed" push callback, so every backend is polled.

**The diff is keyed by (direction, name), not index.** Port indices are not
stable across device-set changes on any backend, so the diff matches ports
by the name `list_ports()` reports (CoreMIDI's bare port name, ALSA's
`"client:port client:port"`, AMIDI's device name) and remembers the last
index seen for removal reporting.

- **Signals** — `midi_port_added(kind, index, name)` and
  `midi_port_removed(kind, index, name)`; `kind` is `"input"`/`"output"`,
  `index` is the current index on add and the last remembered index on
  remove. **Annotated on start**: the constructor's immediate
  `refresh_ports()` fires `midi_port_added` for every device present at
  init (deterministic, independent of the poll interval).
- **`midi_refresh_ports()`** — forces an immediate re-enumeration + diff;
  returns the number of changed ports (added + removed), or -1 if MIDI is
  unavailable / the I/O thread did not respond.
- **`midi_port_poll_interval`** (property, seconds, default 0.5, min 0.01) —
  the re-enumeration cadence; `midi_set_poll_interval()` / 
  `midi_get_poll_interval()`.
- **Removal debounce** — a device (or the whole set) is reported removed only
  after **two consecutive** empty/absent enumerations, so a transient bad
  probe tick (any backend can momentarily return an empty list) does not
  flap ports. A single missing tick is ignored.
- **Auto-close on removal** — open **real-device** ports whose device
  disappeared are auto-closed and reported via
  `midi_port_error(port, "device removed")`. App-owned **non-real** ports
  (virtual ports, the in-process loopback) are *not* auto-closed by the
  diff.
- **Non-goals** — no **auto-reopen** on reconnect (the app re-opens the
  port itself by re-querying `list_ports()`); no **stable port tokens**
  (indices shift, so re-query `midi_list_inputs()`/`midi_list_outputs()`
  after any `midi_port_*`); **no `MidiBackend` interface change** (hotplug is
  entirely router-side, over `list_ports()`).

**Device-free self-test** — `test_project/scripts/test_midi.gd
--midi-hotplug-smoke` drives the add/remove diff without hardware: on host it
opens/closes an app-owned **virtual** port (CoreMIDI/ALSA enumerate it, so
`port_added` on open, `port_removed` on close → full add + remove); on
Android (no AMIDI virtual-port API) it creates the **in-process loopback**,
which enumerates while active → `port_added` (the loopback has no runtime
close, so the removed leg is ctest-covered there). `HOTPLUG_SMOKE_OK` on all
three platforms.

**Real-hardware unplug leg** — `test_project/scripts/test_midi.gd
--midi-hotplug-monitor` keeps the app running and logs every live device
change, auto-opening new input ports so an unplug exercises the auto-close
path. Verified with physical USB-MIDI devices on **all three backends**:
macOS CoreMIDI, the A133 handheld (ALSA, over its dedicated host-only OTG
port), and the RG DS handheld (Android AMIDI, over its host-only OTG port).
A nanoKONTROL2 (two distinct port names) and an FM-1 (same name on both
sides) each produced, per plug, `midi_port_added` for both sides, and per
unplug `midi_port_removed` for both sides **and** auto-close of the open
real-device input port with `midi_port_error(port, "device removed")`. The
(direction, name)-keyed diff handles both the multi-name controller and the
same-name device correctly.

On ALSA, auto-opening a real input makes RtMidi create a `godot-libpd:<dev>
128:0` receive port that appears as an extra output and is removed with the
device on unplug. On Android, opening a real USB-MIDI input goes through the
`AmidiManager` device-open callback (the `com.yellowlab.rtmidi.
MidiDeviceOpenedListener` Java helper, loaded via the app's class loader and
its native method registered with `RegisterNatives`); unplugging a device
stops the input read thread gracefully (a `pollMidi` read error is caught so
it never aborts the process).

### On-device verification status (M2–M4)

- **Android (M2, Anbernic RK3568; M3 regression on Retroid RG DS):**
  `MIDI_SMOKE_OK print=1 note=1 fanout=1` over the in-process loopback,
  including a second-instance (A -> loopback -> B) fan-out with
  per-instance print attribution; clean exit, zero crashes.
- **macOS (M3, CoreMIDI):** `MIDI_SMOKE_OK print=1 note=1` over the IAC
  bus, now driven by `RtMidiHostBackend` (`[MIDI] backend=RtMidi(CoreMIDI)`
  in the smoke output).
- **A133 (M3, ALSA kernel 4.9):** `note_on port=0 ch=0 pitch=60 vel=100`
  looped through the app's own `openVirtualPort` in/out pair +
  `aconnect <out>:0 <in>:0` (two separate `godot-libpd` clients);
  negative test clean (unwired re-send produces no new `note_on`);
  baseline `SMOKE_OK`. Full recipe: `../docs/knulli-build.md` →
  "MIDI I/O (RtMidi — ALSA sequencer)".
- **Hotplug (M4):** A133 (RtMidi ALSA) `HOTPLUG_SMOKE_OK added=1 removed=1`
  (virtual-port add + remove; ALSA reports the full `client:port` name, so
  the smoke matches a name substring); Android (RtMidi) `HOTPLUG_SMOKE_OK
  added=1 removed=0` (in-process loopback — no AMIDI virtual port, no runtime
  loopback close); macOS (RtMidi CoreMIDI) `HOTPLUG_SMOKE_OK added=1
  removed=1`. Device-free smoke on all three; **the auto-close-on-external
  removal path is verified on real hardware on all three platforms** — macOS
  (CoreMIDI), A133 (ALSA, USB OTG), and RG DS (Android AMIDI, USB OTG) with
  physical nanoKONTROL2 + FM-1 plug/unplug — see "Real-hardware unplug leg"
  above.

**Sysex input and CC capture are not exercisable on a device with no MIDI
hardware** (input-only sysex by design; CC needs a real controller) — the
sysex reassembly logic is covered by the host read-stage tests and the CC
path by the macOS IAC + MIDI-Learn flow (M1). Real-hardware I/O is a
deferred milestone.

See `../docs/superpowers/specs/2026-10-01-godot-libpd-android-midi-design.md`
(status: implemented), `../docs/superpowers/specs/2026-10-02-godot-libpd-rtmidi-full-design.md`
(status: implemented), `../docs/superpowers/specs/2026-10-02-godot-libpd-usb-midi-hotplug-design.md`
(status: implemented), and their plans.

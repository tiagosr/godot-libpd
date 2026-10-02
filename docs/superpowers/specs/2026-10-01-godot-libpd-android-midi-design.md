# Godot libpd GDExtension — v2 M2: Android MIDI I/O (RtMidi AMIDI) — Design

**Status:** approved v2 (2026-10-01, supersedes the MidiBridge design) — Tasks 0–3 implemented (2026-10-02); JavaVM capture resolved via ELF scan of libart through /proc/self/mem (see docs/android-build.md); on-device loopback smoke verified on Android 14
**Supersedes:** nothing — extends v1 (2026-09-27) and v2 M1 MIDI (2026-09-30)
**Targets:** Android arm64-v8a (Godot 4.6, official 4.6.2 export templates + gradle
build), tested on Retroid RG DS (API 34). macOS/Linux MIDI (M1, PortMIDI)
unchanged.

## 1. Summary

M1 delivered MIDI I/O (PortMIDI) on macOS + Linux, with a clean
"not available" no-op on Android. M2 makes the same MIDI surface work on
Android.

**Decision (controller + user, 2026-10-01): use RtMidi for the Android
backend, keep PortMIDI for macOS/Linux.**

- PortMIDI has **no** Android backend (its `pm_java/` is the reverse
  direction — a JNI wrapper for Java apps).
- RtMidi (`thestk/rtmidi`, v6.0.0-era master) ships a native
  `ANDROID_AMIDI` backend in `RtMidi.cpp`: NDK `AMidi` C API + JNI,
  written May 2023 (Yellow Labrador), merged upstream, with real fixes
  since (open-path rewrite 2024-07, nullptr guard 2025-02) and an
  in-repo example Android project. CMake option `RTMIDI_API_AMIDI`
  (default-on for Android targets).
- RtMidi's API is a strict improvement over PM's for our layer:
  complete-message callbacks, first-class full-sysex vectors (its AMIDI
  poll loop reassembles F0..F7 chunks — `pollMidi`, `continueSysex`),
  active maintenance, and a MIDI 2.0/UMP roadmap (7.0.0, issue #385)
  that should close the macOS 2026 CoreMIDI generation gap faster than
  upstream PortMIDI.
- Full migration of macOS/Linux to RtMidi is **explicitly deferred** to
  a later milestone: our PM stack is hardened (vendor patches,
  A133 + IAC verified, 9 ctest targets). A migration re-works the
  backend half of M1 and re-verification on all three targets — a tax
  that buys nothing for M2.

M2 therefore: vendor RtMidi (Android-only build), add
`RtMidiAndroidBackend` behind the new `MidiBackend` interface, keep the
GDScript API unchanged. On Android: `midi_available()` true on API 26+;
below that (or on failure) the existing clean no-op contract.

## 2. Facts established during exploration

- RtMidi AMIDI backend (verified in master `23b8cd5`):
  - Uses NDK `AMidi` C API: `AMidiDevice_fromJava`,
    `AMidiInputPort_open` (out direction), `AMidiOutputPort_open` (in
    direction — NDK naming is inverted relative to our semantics).
  - Requires **one** Java class in the APK:
    `com.yellowlab.rtmidi.MidiDeviceOpenedListener` (ctor `(JZ)V`;
    provided in RtMidi's `contrib/java/` and `android/` example app) —
    the device-open callback trampoline.
  - Input: dedicated `pollMidi` pthread started on open; callbacks
    deliver complete messages; sysex reassembled across F0/F7 chunks.
  - Output: `sendByteVector` via the AMidi input-port write path.
  - `openVirtualPort` is **NOT implemented** for Android (returns an
    error string) — Android has no system virtual MIDI device API.
  - Device-open plumbing uses `Looper.getMainLooper()` handlers and the
    `ActivityThread`-derived context (no activity needed from us).
- Godot 4.6 custom-Java mechanism (verified in
  `platform/android/export/export_plugin.cpp`): preset options
  `gradle_build/use_gradle_build` + `gradle_build/gradle_build_directory`
  (default `res://android`) + optional
  `gradle_build/android_source_template`. The Android build template
  (from the source export template) is a full gradle project installed
  into the project; `.java` files under `app/src/main/java` compile into
  the APK.
- Extension Android build already exists: `build.sh --android` (NDK
  arm64-v8a), `.so` at `res://build/android-arm64/libgodot_libpd.so`,
  referenced by the `.gdextension`. `BUILD_PORTMIDI=OFF` on Android
  (stays OFF — PM is not compiled for Android).
- NDK ships `libamidi.so` from platform 26; `minSdk 26` for the MIDI
  build (documented; RG DS = 34).
- RG DS: API 34, arm64-v8a, **has a touchscreen** (unlike the A133) —
  test scene buttons are tappable; **zero expected system MIDI ports**
  (no USB MIDI device attached) → device testing relies on the
  in-process virtual loopback (§3.3), exactly as the A133 relied on
  app-created virtual ports.
- M1 threading invariants (still hold): all `libpd_*` on the instance
  worker thread; Godot signals on the main thread; MIDI I/O on a
  dedicated thread; input bytes enter through the router's ring and are
  drained on the main thread.

## 3. Architecture

```
 GDScript (LibpdServer / test_midi.tscn)              unchanged
   │
 LibpdServer ── MidiRouter ── MidiRoutingTable        unchanged
   │                   │
   │            MidiBackend (NEW interface)
   │            ├── PortMidiBackend     (macOS/Linux; M1 code, byte-identical)
   │            └── RtMidiAndroidBackend (RtMidi AMIDI + in-proc loopback)
   │
 RtMidi (vendored, Android build only) ⇄ NDK AMidi ⇄ android.media.midi
                                                        (one Java listener
                                                         class in the APK)
```

**MidiBackend interface** (extracted from the router's direct `Pm_*`
usage; small and flat):

```cpp
struct MidiBackendPort {
    int index;
    String name;
    bool is_input;
    bool is_output;
};

class MidiBackend {
public:
    virtual bool available() = 0;                       // API 26+ etc.
    virtual Error initialize() = 0;                    // one-time
    virtual Vector<MidiBackendPort> list_ports() = 0;
    virtual Error open_input(int index, int buffer) = 0;
    virtual Error open_output(int index, int buffer) = 0;
    virtual Error write(const uint8_t *bytes, int len) = 0;  // to open output
    virtual Error write_sysex(const uint8_t *bytes, int len) = 0;
    virtual Error close() = 0;
    virtual void shutdown() = 0;
    // in-process loopback (device testing + same-device routing):
    virtual Error create_virtual_loopback(const String &name) = 0; // one in + one out port
};
```

- `PortMidiBackend`: wraps today's router `Pm_*` logic (read-stage state
  machine, virtual ports, bounds-validated opens). Behavior
  byte-identical to M1; the full M1 test suite keeps passing.
- `RtMidiAndroidBackend`:
  - system ports via `RtMidiIn/RtMidiOut` (AMIDI);
  - `write_sysex` via `RtMidiOut::sendSysEx` (PM backend maps this to
    raw `Pm_WriteShort` F0..F7 words — the raw output path already
    handles F0 frames);
  - **in-process virtual loopback** (our layer, since RtMidi
    `openVirtualPort` is unimplemented on Android): the backend
    registers one extra in/out port pair; `write()`/`write_sysex()` on
    the loopback out delivers bytes straight into the input ring
    (bypassing RtMidi) — the Android analogue of the A133 `aconnect`
    wiring. Loopback ports are reported by `list_ports()` with a
    distinctive name prefix.
- Input delivery, both backends: bytes (≤4-byte words for PM; complete
  messages for RtMidi) are pushed into the **same** input ring; main
  thread drains as today; the router's sysex read-stage state machine
  is shared — for RtMidi, the backend pre-chops complete messages into
  ≤4-byte words at the ring boundary so downstream is byte-identical
  across platforms (simplest shared semantics; documented).
- Sysex **input** on Android: in scope (RtMidi reassembles; verified on
  device — see §6). Sysex **output**: in scope via `sendSysEx` (no
  `[sysexout]` in this libpd build, so end-to-end sysex loopback remains
  impossible by design — same as M1; unit-level coverage).

### 3.1 Java in the APK

Exactly one class: `MidiDeviceOpenedListener` (from RtMidi
`contrib/java/`, package `com.yellowlab.rtmidi`), placed in the
installed gradle project
(`test_project/android/app/src/main/java/com/yellowlab/rtmidi/`). No
other custom Java. (License note: RtMidi's license asks that
modifications be sent to the developer; we do not modify it.)

### 3.2 Build & export plumbing

1. `test_project/android/` = installed Android build template (gradle
   project, from the 4.6.2 source template) + the listener class.
2. Android export presets switch to `gradle_build/use_gradle_build =
   true` (debug + release); minSdk 26 for MIDI (documented; RG DS = 34).
3. Extension CMake: `thirdparty/rtmidi` (vendor: `RtMidi.h`,
   `RtMidi.cpp`); Android branch compiles it with
   `RTMIDI_API_AMIDI=ON`, links `amidi` (+ `log`, `android`);
   `midi_backend_rtmidi_android.cpp` joins the `.so`;
   `PORTMIDI_ENABLED` gates only the PM backend; backend selected at
   compile time per platform.
4. `docs/android-build.md` gains the gradle-build + RtMidi procedure
   (SDK/NDK/JDK/gradle versions pinned from the recon task).

### 3.3 Threading (invariants preserved)

| Direction | Thread |
|---|---|
| `libpd_*` / hooks | instance worker thread (unchanged) |
| MIDI output writes | dedicated MIDI I/O thread (unchanged) → `RtMidiOut::sendByteVector/sendSysEx` (thread-safety verified by the multi-instance fan-out test, §6) |
| MIDI input events | RtMidi `pollMidi` thread → backend adapter copies bytes into the ring → **main thread drain** (unchanged contract) |
| Godot signals | main thread (unchanged) |

## 4. Scope, limits, deferred

**In scope (M2):**
- `midi_available()` true on API 26+; full M1 API surface on Android:
  enumerate, open, route, note/CC/PC/pitch/aftertouch/byte in+out,
  typed signals, MIDI-Learn-style CC capture in the test app,
  multi-instance fan-out (same router).
- Sysex input (RtMidi reassembly; on-device test) + sysex output
  (`sendSysEx`; unit-level coverage).
- In-process virtual loopback ports (device testing + same-device
  routing).
- test_midi smoke on-device (virtual loopback; touchscreen taps).

**Out of scope / deferred (clean no-op contract where applicable):**
- **USB-MIDI host specifics** (hotplug churn, permission flows) —
  enumeration is polled like M1's `list_ports()`; hotplug
  auto-reconnect already deferred from M1 for all platforms.
- **Full RtMidi migration on macOS/Linux** — later milestone, when the
  macOS CoreMIDI generation gap or the UMP roadmap matters; re-work
  `PortMidiBackend` → RtMidi + full re-verification (incl. A133 ALSA).
- **iOS/Windows** — unchanged, out of scope (v1 spec §11).
- **minSdk < 26** — MIDI unavailable, app still runs.

## 5. Verification

- **Unit (no device):** backend interface tests (fake backend); all M1
  ctest targets green on macOS after the interface extraction
  (9/9); `MIDI_SMOKE_OK` unchanged.
- **Android cross-compile:** extension `.so` + RtMidi build clean for
  arm64-v8a (NDK); APK builds via gradle.
- **Android on-device (RG DS, API 34):**
  - boot: `midi_available() == true`; port list shows the virtual
    loopback pair (+ any system ports — expected: none);
  - loopback smoke: note written → `[notein]` patch echoes →
    `note_on` signal observed (Android variant of `MIDI_SMOKE_OK`
    using the virtual loopback);
  - **sysex input**: loopback-delivered sysex → complete `F0..F7`
    signal (or documented fallback if reassembly misbehaves on-device);
  - CC capture (MIDI-Learn flow) via touchscreen taps;
  - multi-instance fan-out (2 instances, A→B) on the same router.
- **Negative:** API-26 guard path — `supported()` false ⇒
  `midi_available() == false` (code path covered by the fake-backend
  unit test + review; on a <26 device the app must still run).

## 6. Risks

| Risk | Mitigation |
|---|---|
| Godot gradle-build path on the 4.6.2 templates is the least-proven tooling | **Recon task 0**: ship the single RtMidi listener Java class through the gradle export end-to-end on the RG DS *before* any MIDI work; fail fast on tooling, record working versions in docs |
| RtMidi AMIDI backend is young/community-authored (2023, one author) | Recon task 0 exercises open/list; if it misbehaves, options: (a) fix + contribute upstream (license encourages), (b) fall back to the hand-rolled JNI bridge design (this spec's v1 architecture remains the documented fallback) |
| `sendByteVector` thread-safety from our MIDI I/O thread | Fan-out on-device test (§5) exercises concurrent writes; RtMidi's ALSA/CoreMidi out paths are used cross-thread by default in the wild |
| NDK `amidi` link specifics (platform-26 min, symbol visibility) | Recon task 0 compiles RtMidi AMIDI into the `.so` before wiring the router |
| gradle/JDK versions on the build machine | Pinned in docs by recon task 0 |
| APK size/perms | One small Java class; no new permissions (MIDI needs none) |

## 7. Non-goals

- No PortMIDI Android backend (doesn't exist; see §1).
- No macOS/Linux backend changes beyond the interface extraction
  (behavior-identical, re-verified by the full M1 suite).
- No full RtMidi migration in this milestone (§4).
- No Knulli/A133 work (ALSA MIDI stays M1).

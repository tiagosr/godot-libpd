# Godot libpd — v2 M2: Android MIDI (RtMidi AMIDI) — Implementation Plan

**Spec:** `docs/superpowers/specs/2026-10-01-godot-libpd-android-midi-design.md` (v2, RtMidi hybrid)
**Branch:** `gdext-libpd`
**Process:** SDD (built-in worker + reviewer subagents); HARD VERIFICATION
RULE — every worker claim must paste real command output. Subagent
timeouts 7200000 ms (slow model endpoint). Device: RG DS
(serial `56cda52938004166`, API 34) — user keeps it adb-connected; do
**not** drop adb mid-run (A133 lesson).

## Baseline (controller-verified before M2 start)

- ctest 9/9 (macOS), `MIDI_SMOKE_OK` exit 0, baseline smoke OK
- tree clean at the M1 close; submodules: portmidi `6be63b7`, libpd
  `ba0dc63`, godot-cpp `27d9dd2`
- RG DS connected: API 34, arm64-v8a

## Task 0 — Tooling recon (CONTROLLER, not a worker)

Interactive discovery; fail fast before any MIDI code.

1. Vendor RtMidi: `extension/thirdparty/rtmidi/` (`RtMidi.h`,
   `RtMidi.cpp`, `LICENSE`, `contrib/java/MidiDeviceOpenedListener.java`
   at master `23b8cd5` or later).
2. NDK cross-compile RtMidi alone (CMake, `RTMIDI_API_AMIDI=ON`,
   arm64-v8a, min 26) → `libamidi`/`log`/`android` link check.
3. Install the Godot 4.6.2 Android build template into
   `test_project/android/`; add `MidiDeviceOpenedListener.java` under
   `app/src/main/java/com/yellowlab/rtmidi/`.
4. Android export preset → `gradle_build/use_gradle_build=true`
   (debug; keep release parallel); minSdk 26.
5. Export the debug APK (current extension, MIDI not wired — the Java
   class is inert), install on RG DS, launch, capture logcat:
   - app boots to the existing boot_check scene;
   - no gradle/JNI crashes;
   - class present in the APK (`unzip -l` or dexdump).
6. Probe device MIDI reality: `adb shell dumpsys media.midi` (or
   equivalent) → record available MIDI devices/ports on the RG DS.
7. Record working versions (JDK, gradle, NDK, template) in
   `docs/android-build.md`; recon notes to the ledger.
8. Commits: `chore(android): vendor RtMidi + gradle export recon` (tree)
   + any docs.

**Exit criteria:** APK with the listener Java class runs on the RG DS;
RtMidi AMIDI cross-compiles; gradle procedure documented.

## Task 1 — MidiBackend interface + PortMidiBackend (worker)

Extract `MidiBackend` (spec §3) from the router's direct `Pm_*` usage;
`PortMidiBackend` wraps the existing logic byte-identically.

- Files: `extension/src/midi_backend.h` (interface + port struct),
  `extension/src/midi_backend_portmidi.{h,cpp}` (extraction),
  `extension/src/midi_router.{h,cpp}` (now talks to the interface),
  `extension/tests/midi_backend_fake.cpp` + tests (fake-backend
  routing test), `extension/CMakeLists.txt` (targets only).
- TDD: the 9 existing ctest targets + `MIDI_SMOKE_OK` are the spec —
  all must pass unmodified; add fake-backend tests first.
- Android stays a clean no-op compile (interface present, PM backend
  excluded, router falls back to `midi_available()==false` — same as
  today on Android).
- Review gate: reviewer verifies no behavior change (diff of
  PortMidiBackend vs old router logic) + suite green.

## Task 2 — RtMidiAndroidBackend (worker)

`extension/src/midi_backend_rtmidi_android.{h,cpp}`:

- system ports via `RtMidiIn`/`RtMidiOut` (AMIDI); list/open/close.
- input callback (RtMidi pollMidi thread) → pre-chop complete messages
  into ≤4-byte words → push into the shared input ring (same contract
  as PM backend; sysex arrives complete from RtMidi → chop keeps
  F0..F7 intact).
- `write()` → `sendByteVector`; `write_sysex()` → `sendSysEx`.
- in-process virtual loopback (spec §3): one in+out port pair,
  distinctive name prefix; loopback out write → ring directly.
- `available()` → API 26 check (ro build sdk via `__system_property_get`
  or the JNI-free `android_get_device_sdk`? use
  `__system_property_get("ro.build.version.sdk", ...)`) — no JVM needed.
- Unit-testable seams (fake RtMidi is impractical — test the seams):
  pre-chop function (pure), loopback routing (backend against a stub
  transport), sdk-guard parsing (pure string test).
- CMake: Android branch compiles `thirdparty/rtmidi` (`RTMIDI_API_AMIDI=ON`)
  + backend file into `godot_libpd.so`; links `amidi log android`;
  `PORTMIDI_ENABLED` only gates the PM path.
- Verification: ctest green on macOS (fake seams), Android `.so`
  cross-compiles clean (NDK), no macOS smoke regression.

## Task 3 — Wire + export + boot verification (worker)

- `LibpdServer` MIDI init selects the backend per platform
  (android → RtMidiAndroidBackend; macOS/linux → PortMidiBackend).
- `test_midi.tscn`/`test_midi.gd`: Android smoke variant — virtual
  loopback instead of IAC auto-detect (detect backend name from the
  port list; same `MIDI_SMOKE_OK` contract).
- Export debug APK (gradle), install RG DS, run:
  - `midi_available() == true`;
  - port list shows the loopback pair (+ dumpsys cross-check);
  - boot_check unaffected.
- Commit + ledger with real device output pasted.

## Task 4 — On-device verification + docs (worker)

- loopback note smoke: `MIDI_SMOKE_OK` (android variant) on the RG DS;
- **sysex input**: loopback sysex → complete `F0..F7` signal (record
  outcome; fallback = documented limitation, not a silent pass);
- CC capture via touchscreen taps (MIDI-Learn flow in test_midi);
- multi-instance fan-out (2 instances, A→B) on-device;
- update `docs/android-build.md` (final procedure), `extension/README.md`
  (M2 section: RtMidi hybrid, threading, loopback, sysex status), spec
  status → implemented.
- Final verification by controller: ctest 9/9 (or new count), macOS
  `MIDI_SMOKE_OK`, tree clean.

## Device protocol

- RG DS stays adb-connected through tasks 0, 3, 4.
- Never kill adb mid-run; use `adb -s 56cda52938004166`.
- Logcat capture: `adb logcat -s godotlibpd:* Godot:*` style; screen
  verification via `adb exec-out screencap` when needed (touchscreen
  available — no uinput needed).
- Echo-loop caution (M1 lesson): unwired loopback on an echoing patch
  runs at MIDI rate — always disconnect/teardown at test end.

## Deferred (post-M2, documented)

- Full RtMidi migration (macOS/Linux) — milestone decision when the
  macOS CoreMIDI generation gap or UMP roadmap matters.
- USB-MIDI host hotplug, iOS/Windows, minSdk < 26 devices.

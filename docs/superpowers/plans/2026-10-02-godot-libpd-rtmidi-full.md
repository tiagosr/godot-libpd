# v2 M3 — Full RtMidi Integration Implementation Plan

**Spec:** `docs/superpowers/specs/2026-10-02-godot-libpd-rtmidi-full-design.md`
**Goal:** Make RtMidi the default MIDI backend on macOS (CoreMIDI) + Linux
(ALSA), in addition to Android (AMIDI). PortMIDI stays buildable as a
fallback. All `MIDI_SMOKE_OK` gates keep passing.
**Task 0 recon:** COMPLETE — both platforms GO (see spec §7).

## Conventions (carry over from M1/M2)

- **HARD VERIFICATION RULE:** every claim backed by a command actually run +
  real output.
- **MIDI thread invariants:** all `libpd_*` on instance worker thread; Godot
  signals on main thread; all device writes on the MIDI I/O thread.
- **Sysex:** input-only, whole-message, ≤127 data bytes. RtMidi delivers
  whole `F0..F7`; the read stage re-slices to ≤4-byte words.
- **libpd channel encoding:** `libpd_channel = pd_channel + 16*pd_port`,
  `pd_port` always 0.
- **Subagent timeouts:** 7200000ms (2h) for any delegated task (slow model
  endpoint).
- **A133 staging:** push to `/tmp` (tmpfs), ES suspended for injection tests,
  `aconnect` operands read from `aconnect -l` (RtMidi uses two separate
  clients: "RtMidi Input Client" + "RtMidi Output Client").

## Task 1 — RtMidiBackend (CoreMIDI/ALSA) + factory switch

**Status:** PENDING

New backend reusing the M2 seams. Compile RtMidi for all platforms. Factory
selects `RtMidiBackend` for non-Android hosts; Android keeps
`RtMidiAndroidBackend`.

### 1.1 Files
- `src/midi_backend_rtmidi_core.h` — `RtMidiBackend : MidiBackend` (CoreMIDI/ALSA).
- `src/midi_backend_rtmidi_core.cpp` — implementation.
- `src/midi_backend_factory.cpp` — select `RtMidiBackend` on non-Android.
- `CMakeLists.txt` — build RtMidi for macOS + Linux (not just Android);
  per-platform `RTMIDI_API_CORE`/`RTMIDI_API_ALSA`; add new sources; keep
  PortMIDI behind `BUILD_PORTMIDI` (default ON).
- `tests/midi_backend_fake_tests.cpp` — extend / keep interface contract.

### 1.2 Implementation notes
- Input: per open port, a `RtMidiIn` with `setCallback`; callback
  `chop_to_words(*msg)` → per-port `WordRing` (reuse `rtmidi_seam`).
  `ignoreTypes(false, false, false)`.
- Output: per open port, a `RtMidiOut`. `write`/`write_sysex` →
  `sendMessage`.
- `create_virtual_loopback(name)`: in-process WordRing pair at indices 200
  (in) / 201 (out) — identical to Android; no real ports, works everywhere.
- `list_ports()`: RtMidi `getPortCount()`/`getPortName()` (single source, no
  Android-style unified-index translation).
- `has_host_error()`: `false`.
- `initialize()`: wrap `RtMidiIn`/`RtMidiOut` construction; if the API throws
  (no MIDI server), return `Unavailable` rather than abort.

### 1.3 Verification (hard rule)
- `build.sh --macos` → clean build (RtMidi compiled for macOS).
- `ctest --test-dir build` → all green (fake-backend + seam tests pass).
- `test_midi --midi-smoke` on macOS over IAC →
  `MIDI_SMOKE_OK print=1 note=1` **now driven by `RtMidiBackend`** (confirm
  via a `[MIDI] backend=RtMidi` log line or equivalent).
- Negative: with IAC absent, `midi_open_input(0)` → `midi_port_error`, no
  freeze.

## Task 2 — Linux (A133) + Android regression (COMPLETE)

**Status:** PENDING (blocked by Task 1)

### 2.1 A133
- `build.sh --linux-arm64` (Knulli) → rebuild the engine+extension with
  `RtMidiBackend`.
- Stage to `/tmp` (tmpfs), ES suspended, run `test_midi`.
- Open virtual in + out via the app (RtMidi creates two clients), read
  `aconnect -l`, wire `aconnect <out-client>:0 <in-client>:0`, send test note
  → looped `note_on` (+ sysex) in the log. Unwire when done.
- Confirm the app's `aconnect` operands are the two separate RtMidi clients
  (not PortMIDI's single client).

### 2.2 Android
- Rebuild extension `.so`, refresh into
  `test_project/android/build/libs/debug/arm64-v8a/`, `gradlew`, sign with
  apksigner, install on the Anbernic (serial `56cda52938004166`) when
  available.
- `MIDI_SMOKE_OK print=1 note=1 fanout=1` over the in-process loopback
  (backend unchanged — Android regression check).

### 2.3 Baseline
- `SMOKE_OK` (audio) unaffected on macOS + A133.

## Task 3 — Docs (COMPLETE)

**Status:** PENDING (blocked by Task 1, 2)

- `extension/README.md`: update "MIDI backends" section — RtMidi is now the
  default on macOS/Linux/Android; PortMIDI is a fallback flag; document the
  ALSA separate-client `aconnect` operand change; note sysex is now
  whole-message from RtMidi on all platforms.
- `docs/superpowers/specs/2026-10-02-...md` → status: implemented.
- `docs/knulli-build.md`: MIDI section reflects RtMidi (ALSA) + the
  two-client `aconnect` operands.
- SDD ledger for M3.

## Task breakdown / sequencing

1. **Task 1** (backend + factory + build) — the core; gate on macOS
   `MIDI_SMOKE_OK`.
2. **Task 2** (A133 + Android regression) — on-device; needs A133 + Anbernic.
3. **Task 3** (docs) — last.

## Rollback

RtMidi default is one factory branch; `MIDI_BACKEND=portmidi` (or rebuilding
with the flag) restores PortMIDI on macOS/Linux. PortMIDI sources are not
deleted in M3, so rollback is a flag flip.

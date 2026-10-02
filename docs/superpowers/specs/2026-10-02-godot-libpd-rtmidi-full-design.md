# v2 M3 — Full RtMidi Integration (CoreMIDI + ALSA behind MidiBackend)

**Status:** proposed (2026-10-02)
**Supersedes:** nothing — extends M1 (PortMIDI) + M2 (RtMidi Android) by making
RtMidi the default MIDI backend on **all** platforms.
**Task 0 recon:** COMPLETE (2026-10-02), both platforms GO — see §7.

## 1. Goal

Make the vendored RtMidi the MIDI backend on macOS (CoreMIDI) and Linux (ALSA
sequencer), in addition to the already-shipped Android (AMIDI) backend.
PortMIDI remains available behind a build flag as a fallback, not deleted.

After M3, the extension has **one** MIDI backend family (RtMidi) selected by
platform, with the same `MidiBackend` interface, router, read stage, and
GDScript API as M1/M2. The only backend-specific surface is port
enumeration, open/close, write, and `write_sysex`.

## 2. Why now

M2 already moved Android to RtMidi behind the `MidiBackend` interface and
proved the seams (per-port `WordRing`, `chop_to_words`, in-process loopback,
throw-based error handling). The remaining PortMIDI surface on macOS/Linux is
exactly what a `RtMidiBackend` (non-Android) replaces. Task 0 de-risked the
two known unknowns:

- **CoreMIDI "generation gap"** (the original hold reason): probe shows
  CoreMIDI enumerates the IAC bus, opens both directions, round-trips a note
  and a full `F0..F7` sysex, and `openVirtualPort()` works in both
  directions. No gap observed.
- **ALSA virtual ports on A133 kernel 4.9**: probe shows RtMidi's
  `openVirtualPort()` creates real snd_seq ports on both directions, and an
  `aconnect` wire-back delivers a note **and** a full 7-byte sysex. This is
  the exact loopback path the A133 test app uses.

## 3. Architecture

```
                +------------------------------------------+
   GDScript --->| LibpdServer  (server-wide MIDI ownership)|
                +----------------------+-------------------+
                                       |
                              +--------v---------+
                              |     MidiRouter    |  (MIDI I/O thread)
                              +--------+----------+
                                       |  MidiBackend (interface)
                 +---------------------+--------------------+
                 |                                      |
      +----------v-----------+              +------------v-----------+
      |   RtMidiBackend      |              | RtMidiAndroidBackend   |
      | (CoreMIDI / ALSA)    |  NEW         | (AMIDI; M2, unchanged) |
      +----------------------+              +------------------------+
```

`RtMidiBackend` (new, `src/midi_backend_rtmidi_core.cpp/.h`) is the
non-Android RtMidi backend. It reuses the platform-free seams from M2
(`rtmidi_seam::chop_to_words`, `rtmidi_seam::WordRing`) so the input
callback → per-port ring → router read stage is identical to Android.

### Backend contract (unchanged — `MidiBackend`)

| Method | RtMidiBackend behavior |
|---|---|
| `initialize()` | Construct `RtMidiIn`/`RtMidiOut` with the platform API; return `Unavailable` if the API init throws (no MIDI server). |
| `list_ports()` | RtMidi's own `getPortCount()`/`getPortName()` (unified index space — RtMidi is the single source, so no Android-style translation needed). |
| `open_input(idx)` | One `RtMidiIn` object per open port; `setCallback` → `chop_to_words` → per-port `WordRing`. `ignoreTypes(false,false,false)`. |
| `open_output(idx)` | One `RtMidiOut` object per open port. |
| `write(handle, bytes)` | `RtMidiOut::sendMessage()` (full-form 3-byte short msgs). |
| `write_sysex(handle, bytes)` | `RtMidiOut::sendMessage()` (whole `F0..F7`; RtMidi delivers it as one message). |
| `create_virtual_loopback(name)` | **In-process** loopback pair (indices 200 in / 201 out) backed by a `WordRing` — identical to Android; no real ports created, so it works on every platform. |
| `has_host_error()` | `false` (RtMidi has no queryable per-stream error state). |
| `shutdown()` | Close all ports, clear maps, reset loopback. |

### Delivery model (unchanged — option C)

Every incoming event is delivered **dual**: high-level (`libpd_noteon` →
`[notein]`, `libpd_cc` → `[ctrl]`, …) **and** raw bytes to `[midiin]`
(`libpd_midibyte`). Sysex arrives whole-message (RtMidi reassembles `F0..F7`)
and is surfaced as the typed `midi_sysex(port, data)` signal; the read stage
still re-slices it into ≤4-byte words for the raw-byte path. **Input-only**
sysex is retained (no `[sysexout]` in this libpd build).

### Threading (unchanged from M1/M2)

All `libpd_*` on each instance's worker thread; all device writes
(`sendMessage`) on the single MIDI I/O thread; RtMidi input callbacks fire on
RtMidi's internal thread and only copy bytes into the per-port `WordRing`;
the router I/O thread drains and routes. Godot events cross to the main
thread as typed signals.

## 4. Platform notes

- **macOS / CoreMIDI** — RtMidi's `MACOSX_CORE` API. Sysex arrives reassembled
  (verified). `openVirtualPort()` works (verified) but the test app uses IAC,
  so the app-created virtual-port path is not needed on macOS.
- **Linux / ALSA** — RtMidi's `LINUX_ALSA` API. The A133 exposes **zero**
  SUBS-capable seq ports, so the test app's loopback uses
  `openVirtualPort()` (in + out, on separate RtMidi clients) + `aconnect`
  wire-back — the same procedure as M1's PortMIDI virtual ports, just created
  by RtMidi. Note: RtMidi creates the in- and out- ports on **separate**
  clients (unlike PortMIDI, which used one client / two ports), so the
  `aconnect <out-client>:0 <in-client>:0` operands differ — read them from
  `aconnect -l`.
- **Android / AMIDI** — unchanged (M2). The factory still selects
  `RtMidiAndroidBackend` on `__ANDROID__`; `RtMidiBackend` is compiled only
  for non-Android hosts.

## 5. Build & selection

- `extension/CMakeLists.txt`: build RtMidi for **all** platforms (not just
  Android). `RTMIDI_API_CORE` (Apple) / `RTMIDI_API_ALSA` (Linux) enabled
  per platform. PortMIDI stays buildable behind `BUILD_PORTMIDI` (default ON
  for now as a fallback; `PORTMIDI_ENABLED` still defined when built).
- `src/midi_backend_factory.cpp`:
  - `__ANDROID__` → `RtMidiAndroidBackend` (unchanged).
  - else → `RtMidiBackend` (default). PortMIDI reachable only if a build
    sets a new `MIDI_BACKEND=portmidi` option (escape hatch; default
    `rtmidi`).
- `build.sh`: no new flags needed for the default; keep `--macos/--linux-arm64/
  --android`.

## 6. Testing & verification

- **ctest** — `midi_backend_fake_tests` still pins the interface contract
  (backend-agnostic). `midi_rtmidi_seam_tests` still covers `chop_to_words` /
  `WordRing`. A new `RtMidiBackend` unit (or extending the fake-backend
  target) covers the CoreMIDI/ALSA backend's port-handle bookkeeping where
  it is testable without a live MIDI server.
- **macOS smoke** — `test_midi --midi-smoke` over IAC must still produce
  `MIDI_SMOKE_OK print=1 note=1`, now driven by `RtMidiBackend` (CoreMIDI).
- **Linux (A133) smoke** — the app's virtual-port + `aconnect` loopback must
  still produce a looped `note_on` (+ sysex) with `RtMidiBackend` (ALSA).
- **Android regression** — `MIDI_SMOKE_OK print=1 note=1 fanout=1` over the
  in-process loopback (unchanged backend).
- **Baseline** — `SMOKE_OK` (audio) unaffected on all platforms.

## 7. Task 0 recon evidence (2026-10-02)

`probes/rtmidi_probe_macos.cpp` (CoreMIDI, run on the dev Mac, IAC Bus 1):
```
P1 in_count=1 out_count=1          (IAC Driver Bus 1 visible both sides)
P2 open_in(0) OK / open_out(0) OK
PROBE note_on ch=0 pitch=60 vel=100 (nbytes=3)
PROBE sysex nbytes=7 first=f0 last=f7      (full F0..F7 reassembled)
P3 in.openVirtualPort OK / out.openVirtualPort OK
P2/P4 got_note=60 got_sysex_bytes=7
```
`probes/rtmidi_probe_linux.cpp` (ALSA, cross-built in the Knulli Docker
builder, run on the A133; `aconnect 129:0 128:0` wired while live):
```
P1 in_count=0 out_count=0          (no SUBS-capable ports, as expected)
P2 in.openVirtualPort OK / out.openVirtualPort OK
   -> aconnect -l: client 128 "RtMidi Input Client" / 129 "RtMidi Output Client"
PROBE note_on ch=0 pitch=60 vel=100 (nbytes=3)
PROBE sysex nbytes=7 first=f0 last=f7
P4 got_note=60 got_sysex_bytes=7   (loopback round-trip confirmed)
```
Decision: **GO** on both platforms.

## 8. Risks & mitigations

- **RtMidi CoreMIDI enumeration gaps** on hardware beyond IAC — mitigated by
  keeping PortMIDI buildable (`MIDI_BACKEND=portmidi`) and by the macOS smoke
  gate; any future CoreMIDI regression is a flag flip, not a rewrite.
- **A133 separate-client `aconnect` operands** — the test docs updated to
  read the actual client numbers from `aconnect -l` (RtMidi uses two clients,
  not PortMIDI's single client/two ports).
- **Two clients vs one** on ALSA — no functional difference for loopback;
  only the `aconnect` operand format changes.

## 9. Deferred (out of scope for M3)

- Full deletion of PortMIDI (kept as fallback).
- USB-MIDI hotplug, iOS/Windows, minSdk < 26.
- RtMidi upstream PR (the 3 Android fixes + any new ones) — separate effort.
- The M1-flagged raw-byte running-status truncation (independent of backend).

# godot-libpd v2 (M1): MIDI I/O via PortMIDI — Design

Status: implemented (M1 delivered; A133 ALSA-sequencer loopback verified on-device 2026-10-01 — see `docs/knulli-build.md` "MIDI I/O")
Supersedes nothing; extends `2026-09-27-godot-libpd-gdextension-design.md`
(the v1 spec listed "MIDI I/O via PortMIDI" as the first v2 item).

## 1. Purpose & success criteria

Give `godot-libpd` full external MIDI I/O on **macOS and Linux**:

- Open/close PortMIDI input and output devices from GDScript.
- **System-level routing** (owned by `LibpdServer`, not by patches):
  an input port fans out to N `LibpdInstance`s; an instance's MIDI output
  fans out to N output ports. Patches are bespoke; routing is a system
  concern.
- Every decoded input event is available **two ways, simultaneously**
  (agreed option "C"):
  1. into pd — both high-level (`libpd_noteon`/... → `[notein]`/`[ctlin]`/...)
     and raw byte (`libpd_midibyte` → `[midiin]`, `libpd_sysex` → `[sysexin]`);
  2. into **GDScript** as typed signals on `Libpd`, so the Godot side can
     capture events (e.g. CC for **MIDI-Learn**) and manage assignments
     without complicating patches.
- Success: a patch with `[notein]`/`[midiin]` responds to a real MIDI
  stream; a patch with `[noteout]`/`[ctlout]`/`[midiout]` reaches a real
  output device; GDScript receives the same events for CC capture;
  multi-instance fan-out verified; nothing on the audio path can block
  on MIDI I/O.

**Out of scope (this milestone):** Android (PortMIDI needs its
Java/JNI backend — API exists but returns a clean "not available" error),
channel filtering per route, sysex *output* (no `[sysexout]` object in
this libpd build), device hotplug auto-reconnect, Godot built-in
`MIDIInput` classes (engine built without PortMIDI; the extension is the
MIDI layer).

## 2. Facts established during exploration

- PortMIDI 2.0.8 (`extension/thirdparty/portmidi` @ `6b51c25`) is already
  compiled into the extension on macOS (CoreMIDI) and Linux (ALSA
  sequencer); `BUILD_PORTMIDI=OFF` on Android (JNI layer not wired).
- libpd input paths (do not cross-feed):
  - high-level: `libpd_noteon/controlchange/programchange/pitchbend/
    aftertouch/polyaftertouch` → `[notein]`/`[ctlin]`/`[pgmin]`/...
  - raw: `libpd_midibyte(port, byte)` → `[midiin]`; `libpd_sysex(port,
    byte)` → `[sysexin]`.
- libpd output hooks (all fire on the pd/DSP thread — which in this
  architecture is the **instance worker thread**, since
  `libpd_process_float` runs there):
  - `libpd_set_noteonhook` ← `[noteout]`, `controlchangehook` ← `[ctlout]`,
    `programchangehook` ← `[pgmout]`, `pitchbendhook` ← `[bendout]`,
    `aftertouchhook` ← `[touchout]`, `polyaftertouchhook` ← `[polytouchout]`,
    `midibytehook` ← `[midiout]`.
  - Hooks must (un)set only while DSP is stopped; they are installed once
    at pd-instance init and never changed afterwards.
  - There is **no sysex output hook** and no `[sysexout]` class in the
    compiled pd (`x_midi.c` defines `[midiin] [sysexin] [notein] [ctlin]
    [pgmin] [bendin] [touchin] [polytouchin] [midi realtime in]` and the
    output side without sysex).
- libpd channel encoding: `libpd_channel = pd_channel + 16 * pd_port`
  (0-indexed channel in low nibble). Our instances are single-pd-port:
  always `pd_port = 0`; on output we decode `channel & 15`.
- The worker's command queue `pop` wakes on new items even while the
  instance is idle (DSP off) — MIDI routed to an idle instance works.
- The ALSA backend exposes the kernel sequencer's virtual ports, so on
  the A133 (no physical MIDI) input is testable via `aconnect` between
  two of the app's own virtual ports. macOS test path: CoreMIDI IAC
  Driver bus loopback (open the same bus as input and output).

## 3. Public API (GDScript, all on the `Libpd` singleton)

```gdscript
# discovery / lifecycle
Libpd.midi_available() -> bool                      # false on Android
Libpd.midi_list_inputs()  -> Array                  # [{index: int, name: String}]
Libpd.midi_list_outputs() -> Array
Libpd.midi_open_input(index: int) -> int            # port id (>= 0); -1 on error
Libpd.midi_open_output(index: int) -> int
Libpd.midi_close_input(port_id: int) -> void
Libpd.midi_close_output(port_id: int) -> void

# routing (fan-out both directions; add := false unroutes)
Libpd.midi_route_input(port_id: int, instance: LibpdInstance, add := true)
Libpd.midi_route_output(instance: LibpdInstance, port_id: int, add := true)
```

Typed event signals (decoded input; fire on the **main thread**):

```gdscript
Libpd.midi_note_on(port_id, channel, pitch, velocity)
Libpd.midi_note_off(port_id, channel, pitch, velocity)
Libpd.midi_cc(port_id, channel, controller, value)        # MIDI-Learn capture
Libpd.midi_program_change(port_id, channel, program)
Libpd.midi_pitch_bend(port_id, channel, value)            # 14-bit, 0..16383
Libpd.midi_aftertouch(port_id, channel, value)
Libpd.midi_poly_aftertouch(port_id, channel, pitch, value)
Libpd.midi_sysex(port_id, data: PackedByteArray)
```

Semantics:

- Opening a device never starts routing; a port and an instance interact
  only once routed. The same PM device index may be opened multiple times
  (independent port ids).
- **Script listening is independent of pd routing**: signals fire for
  every decoded event of every open input, routed or not.
- Routing is by `LibpdInstance` Node reference; freeing the instance
  auto-unroutes and frees its queues.
- A patch listening on *both* `[notein]` and `[midiin]` for the same
  message sees it twice — inherent to the agreed "both" delivery model.
- Android: all calls return cleanly with an error; `midi_available()`
  is false. No Android-specific code paths in the API surface.

## 4. Architecture & threading

Four threads; one is new. No pd call crosses into a new thread; no MIDI
I/O crosses into the audio path.

```
PM input thread (PortMIDI's own)
    PmReadLong → raw bytes (sysex captured whole)
    → per-port input ring (mutex-protected, unbounded; value copies only)

MIDI I/O thread (NEW, owned by LibpdServer via MidiRouter)   [polls @ ~1 ms]
    1) per port: run running-status framer over the byte ring
       → high-level events + raw byte pass-through + whole sysex
    2) emit decoded events into the main-thread event ring (script signals)
    3) for each (port → instance) route: push MIDI_* commands into the
       instance's existing worker command queue
    4) drain each instance's output queue → re-encode → PmWriteShort /
       PmWriteLong to each (instance → port) route
    5) execute control ops queued from the main thread (closes, unroutes)

Instance worker thread (existing, per instance)
    executes MIDI_* commands: libpd_noteon / libpd_controlchange / ... /
    libpd_midibyte / libpd_sysex   (pd-on-worker contract preserved)
    pd output hooks (installed once at pd init, before DSP) push messages
    into the per-instance output queue (mutex vector, bounded,
    drop-oldest + warning on overflow)

Main thread
    Libpd._process(): drain the main-thread event ring → emit typed
    signals (Godot signals must be main-thread; 60 Hz granularity is
    fine for UI/MIDI-Learn and does not touch the fast path)
```

Invariants:

- **Input never drops:** the PM thread only value-copies bytes into an
  unbounded ring; all parsing/routing is off the PM thread.
- **Audio never blocks on MIDI:** `PmWrite` (the one call that can block
  on a full ALSA queue / missing consumer) happens only on the MIDI I/O
  thread. The worker is never touched by PortMIDI.
- **Idle instances receive MIDI:** worker `pop` wakes on queued commands.
- **No close/write races:** `Pm_Terminate`/`Pm_Close` execute on the MIDI
  I/O thread via a control queue; the API call waits (≤ 500 ms) and
  reports timeout as an error.

## 5. Components & files

New (extension/):

- `src/core/pd_midi_message.h(.cpp)` — POD types:
  - `MidiShortMsg { enum Kind { NOTE_ON, NOTE_OFF, CC, PROGRAM_CHANGE,
    PITCH_BEND, AFTERTOUCH, POLY_AFTERTOUCH } ; uint8 channel, d1, d2 }`
    (pitch bend: d1 = low 7 bits, d2 = high 7 bits → 14-bit value).
  - `MidiSysex { uint8 data[128]; int len }` (127 usable bytes + room;
    longer input is truncated with a warning — System Realtime max is 128).
  - `MidiOutMsg` — union of the above plus raw-bytes for `[midiout]`
    (single `uint8 byte`).
- `src/midi_router.h/.cpp` — the whole subsystem:
  - PortMIDI init/teardown, port tables
    (`MidiInPort { id, PmInputHandle, name }`, `MidiOutPort { id,
    PmOutputHandle, name, per-source running-status state }`),
    routing tables (port → instances; instance → ports),
    input rings, per-instance output queues (registered at worker
    start/teardown), the framer (per-port running-status state),
    the MIDI I/O thread loop, the control queue.
- `tests/midi_framer_tests.cpp`, `tests/midi_router_tests.cpp` — host
  unit tests (see §8); framer and routing table are written as pure,
  PM-independent units so they test without PortMIDI.

Modified:

- `src/libpd_server.{h,cpp}` — own the `MidiRouter`; expose the API +
  signals; `_process()` drains the main-thread event ring; shutdown order
  (§7).
- `src/libpd_worker.{h,cpp}` — new `PdCommand` opcodes (`MIDI_NOTE`,
  `MIDI_CC`, `MIDI_PROGRAM_CHANGE`, `MIDI_PITCH_BEND`, `MIDI_AFTERTOUCH`,
  `MIDI_POLY_AFTERTOUCH`, `MIDI_BYTE`, `MIDI_SYSEX`) carrying the POD
  payloads; the **existing** `PdCommand::MIDI` opcode (packed note, used
  by the v1 `send_midi()` API) stays untouched; hook installation in the
  INIT command path (before DSP can run); per-instance output queue +
  hook trampolines.
- `src/core/pd_command_queue.h` — opcodes + fixed-size payload
  (128-byte sysex field; ring stays small, e.g. 1024 slots ≈ 130 KB
  worst case — acceptable; verify current capacity and bump if needed).
- `CMakeLists.txt` — new sources; `PORTMIDI_ENABLED` compile definition
  when `BUILD_PORTMIDI` (gates the API bodies so Android builds compile
  the same headers with inert implementations).
- `test_project/` — `scenes/test_midi.tscn` + `scripts/test_midi.gd`
  (see §8).
- `docs/knulli-build.md` + this spec's status line.

## 6. Parsing & encoding details

**Input framer (per open port, MIDI I/O thread):** consumes raw bytes;
maintains running-status state. For each byte:

- status byte (0x80–0xFE): sets running status; if it starts a 2/3-byte
  message, buffer data bytes until complete.
- on message completion:
  - note on/off (0x90/0x80): high-level `NOTE_ON` (vel 0 → `NOTE_OFF`) +
    raw bytes pass through.
  - CC (0xB0): high-level `CC` + raw pass-through.
  - program change (0xC0, 2 bytes): `PROGRAM_CHANGE` + raw.
  - pitch bend (0xE0): `PITCH_BEND` (14-bit) + raw.
  - aftertouch ch (0xD0) / poly (0xA0): high-level + raw.
  - `0xF0` sysex: collect until `0xF7` → one `MIDI_SYSEX` (→ pd
    `libpd_sysex` per byte; → GDScript `midi_sysex`). Sysex bytes are
    **not** emitted as raw pass-through (would corrupt `[midiin]`
    framing). > 127 bytes → truncate + warning.
  - realtime (0xF1/0xF3/0xF5, single byte): raw pass-through only (fed
    to `[midi realtime in]`), no high-level signal.
  - other system common (0xF2/F4/F6-F7 outside sysex): raw pass-through
    only, no signal.
- Every non-sysex byte is also emitted as a `MIDI_BYTE` command to each
  routed instance, in original order (running status intact) →
  `libpd_midibyte` → `[midiin]` sees the pristine stream.
- High-level events go to routed instances via their specific opcodes →
  `libpd_*` with `pd_port = 0`.
- The same decoded events are copied into the main-thread event ring
  (signal delivery), independent of routing.

**Output encoding (MIDI I/O thread):** per (instance, port) route:

- high-level hooks → `PmWriteShort` (status/d1/d2). Channel from
  `hook_channel & 15`.
- `[midiout]` raw bytes → per-route running-status writer: emit status
  byte only when it changes (savings + correct stream), data bytes
  always; `PmWriteLong` with 32-byte chunks.
- No sysex output path (documented limitation, §1).

**Channel encoding:** input channels arrive 0–15 from the device; we
pass them to libpd unchanged with `pd_port = 0` (so `[notein 5]` style
addresses work directly). Output hooks return `pd_channel + 16*pd_port`;
we use `& 15` (patches using port-nibble > 0 are out of scope —
documented).

## 7. Lifecycle, errors, shutdown

- **Instance freed while routed:** worker teardown (worker thread)
  notifies the router → unroute + drop output queue (router operations
  are mutex-guarded; the MIDI I/O thread sees the change at its next
  poll).
- **Close input:** auto-unroute everything on that port → control-queue
  op on MIDI I/O thread: `Pm_AbortInput` (stops callback) → `Pm_Close` →
  drop ring/framer state. API waits ≤ 500 ms; timeout → `midi_port_error`.
- **Close output:** auto-unroute → control op: `Pm_Flush` → `Pm_Close`
  (after the thread is done writing to it — same-thread ordering makes
  this race-free).
- **Device unplugged:** next `PmWrite` → `PmTerminated` → emit
  `midi_port_error(port_id, ...)`, auto-close, unroute. Input side: PM
  callback simply stops (no further events).
- **`PmWrite` blocking** (queue full, no consumer): bounded by the ALSA
  queue size (open outputs with a modest `queue` parameter, e.g. 256
  events). It can still block the MIDI I/O thread — acceptable and
  documented; worker output queues then back up and drop-oldest with a
  warning, so audio is unaffected.
- **Error model:** synchronous failures return -1 + `push_error`;
  async failures emit `midi_port_error(port_id, what)`. No asserts on
  user paths (v1 spec §10).
- **Shutdown order** (server deinit): stop routing tables → control op:
  terminate+close all ports → join MIDI I/O thread → (instances' workers
  tear down separately as before; their hooks are cleared by libpd
  instance free).

## 8. Testing

1. **Host unit tests** (`core_tests` harness, no PortMIDI needed):
   - framer: note/CC/PC/PB/AT/poly, running status, sysex capture and
     truncation, realtime interleaving, multi-message streams.
   - channel encode/decode; routing table fan-out add/remove/auto-
     unroute semantics.
2. **macOS integration (automated):** CoreMIDI IAC Driver bus — open one
   IAC bus as both a server input and a server output; route
   instance-out → bus (IAC loops the bus back to its input) → instance-in.
   Verify: `[noteout]`/`[ctlout]` bytes in a test patch arrive as
   `midi_note_on`/`midi_cc` signals and reach a second (or the same)
   instance's `[notein]`/`[ctlin]` (patch `print`s them → existing print
   pipeline).
3. **Knulli on-device (manual):** app opens a PM input (ALSA sequencer
   virtual port) and a PM output; `aconnect <out-virtual>:<in-virtual>`
   from the shell; on-screen event log + patch response prove the full
   path on target.
4. **Test app** (`test_midi.tscn`/`test_midi.gd`): port lists, open/close,
   route buttons, live **Label-based** event log (A133 RichTextLabel
   lesson), "send test note" via the existing `send_midi()` API, and a
   minimal MIDI-Learn demo field (captures the first `midi_cc` and shows
   controller/value).

## 9. Risks & open notes

- **CoreMIDI IAC availability on the build mac:** the IAC Driver ships
  with macOS but may have no buses configured; the integration test must
  create/listen for a bus or skip with a clear message. (Check in plan.)
- **PmWriteLong on ALSA with 32-byte chunks** and per-route running
  status: keep the writer tiny and unit-tested via the host tests
  (the framer test doubles as the writer test through a shared byte
  framing utility if that stays clean; otherwise writer logic lives in
  the framer unit).
- **Command queue sizing** with the 128-byte sysex field — verify
  current `PdCommandQueue` capacity; bump slot count if the worst-case
  memory exceeds ~256 KB.
- **iOS/Windows** remain out of scope entirely (v1 spec §11 unchanged).

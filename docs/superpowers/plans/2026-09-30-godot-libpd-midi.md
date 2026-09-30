# godot-libpd v2 M1: MIDI I/O via PortMIDI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full external MIDI I/O (in + out, system-level routing, GDScript event signals) for the godot-libpd extension on macOS and Linux.

**Architecture:** A `MidiRouter` owned by `LibpdServer` runs one new MIDI I/O thread. PortMIDI input callbacks only copy raw bytes into per-port rings; the MIDI I/O thread runs a pure `MidiFramer` (running-status parsing), fans decoded events out to routed instances' existing worker command queues (all `libpd_*` calls stay on worker threads) and writes captured pd output (worker-thread hooks → per-instance output queues) to PortMIDI. Decoded events also reach GDScript as typed signals, drained on the main thread in `LibpdServer::_process`.

**Tech Stack:** C++17, PortMIDI 2.0.8 (`extension/thirdparty/portmidi`, already compiled), libpd (`extension/thirdparty/libpd`), godot-cpp (GDExtension), CMake/Ninja, CTest. Host tests are plain assert-style executables (existing `core_tests` pattern).

**Spec:** `docs/superpowers/specs/2026-09-30-godot-libpd-midi-design.md`

## Global Constraints

- Godot 4.6; godot-cpp **must** build with `-DGODOTCPP_TARGET=template_release` (see `extension/build.sh` header).
- Thread invariants (spec §4): all `libpd_*` calls on the instance worker thread only; Godot signals emitted on the main thread only; **all `PmWrite*` calls on the MIDI I/O thread only**; the PM input callback does nothing but copy bytes into the ring.
- The existing `PdCommand::MIDI` opcode (v1 `send_midi()` API) stays untouched (spec §5).
- Platforms: macOS (CoreMIDI) and Linux (ALSA) fully functional; Android compiles with `BUILD_PORTMIDI=OFF` and every `midi_*` API returns cleanly with a `push_error` ("MIDI not available on this platform"), `midi_available()` → `false`.
- Sysex: input-only; whole-message; **127 data bytes max**, longer → truncate + warning (spec §6). No sysex output (no `[sysexout]` in this libpd build).
- libpd channel encoding: `libpd_channel = pd_channel + 16 * pd_port`; instances are single-pd-port → always `pd_port = 0`; output hooks decoded with `channel & 15` (spec §6).
- Note-on with velocity 0 is reported as note-off (MIDI convention, spec §6).
- Test app: **plain `Label` for logs, never `RichTextLabel`** (A133 text-shaping spin); GDScript uses `#` comments only (Godot 4.6 has no `//`).
- Knulli on-device work: ES suspended during tests, tmpfs staging, no exfat reads in the launch path (see `docs/knulli-build.md`).
- Commit after every task. Branch: `gdext-libpd`.

## Review Focus

Failure modes the spec implies but no automated task test exercises — each must be consciously checked by the task's reviewer:

1. **Device unplugged mid-write** → next `PmWrite` returns `PmTerminated`; router must emit `midi_port_error`, auto-close, unroute — never crash or hang the I/O thread. (Task 4 reviewer: trace every `PmWrite*` return path; Task 7 on-device test covers the input side via `aconnect` disconnect.)
2. **Instance freed while the MIDI I/O thread is mid-fan-out** → router must tolerate a vanishing `LibpdInstance*` (lookups under the server's mutex; no raw pointer held across the fan-out loop). (Tasks 5 and 7 reviewers.)
3. **`PmWrite` to a port with no consumer** (ALSA queue full) blocks the MIDI I/O thread → outputs open with a bounded `queue` parameter (256 events, spec §7) and worker output queues drop-oldest with a warning, so audio is unaffected. (Task 4 reviewer: verify the queue-size parameter is actually passed in `Pm_OpenOutput`.)
4. **Garbage bytes with no running status at stream start** → framer drops data-only bytes, no crash, state stays sane. (Covered by Task 1 tests; re-check in Task 4 where the framer is fed live ring bytes.)
5. **macOS IAC bus absent** (fresh Mac, no IAC buses configured) → `--midi-smoke` must skip with a clear printed reason and exit 0, not fail the build. (Task 6.)

---

### Task 1: `MidiFramer` — pure running-status parser

**Files:**
- Create: `extension/src/core/pd_midi_framer.h`, `extension/src/core/pd_midi_framer.cpp`
- Test: `extension/tests/midi_framer_tests.cpp`
- Modify: `extension/CMakeLists.txt` (add framer to `libpd_core_tests` sources)

**Interfaces:**
- Consumes: nothing (pure unit, no PortMIDI/libpd).
- Produces (used by Tasks 4–5):
  - `enum class MidiKind : uint8_t { NOTE_ON, NOTE_OFF, CC, PROGRAM_CHANGE, PITCH_BEND, AFTERTOUCH, POLY_AFTERTOUCH };`
  - `struct MidiShortMsg { MidiKind kind; uint8_t channel; uint8_t d1; uint8_t d2; };` (PITCH_BEND: d1 = low 7 bits, d2 = high 7 bits; AFTERTOUCH: d1 = value, d2 unused; CC: d1 = controller, d2 = value; NOTE_ON/OFF: d1 = pitch, d2 = velocity; PROGRAM_CHANGE: d1 = program; POLY_AFTERTOUCH: d1 = pitch, d2 = value)
  - `struct MidiFramingSink { virtual void on_short(const MidiShortMsg &); virtual void on_byte(uint8_t); virtual void on_sysex(const uint8_t *data, int len); virtual void on_sysex_truncated(); }`
  - `class MidiFramer { public: explicit MidiFramer(MidiFramingSink &p_sink); void feed(uint8_t p_byte); void reset(); };` (non-owning sink reference; caller keeps the sink alive for the framer's lifetime — router-side). — one framer per open input port; `feed()` may invoke the sink multiple times per byte (byte passthrough + short message on completion); `on_byte` is called for every non-sysex byte in original stream order (running status included) and is never called for bytes inside an F0..F7 sysex.
- CMake: `libpd_core_tests` gains `src/core/pd_midi_framer.cpp` and a new `add_test(NAME midi_framer_tests COMMAND midi_framer_tests)` executable `tests/midi_framer_tests.cpp + src/core/pd_midi_framer.cpp`.

- [ ] **Step 1: Write the failing tests** — `extension/tests/midi_framer_tests.cpp` (same CHECK-harness style as `core_tests.cpp`). Recording sink collects into vectors. Cases:
  - `note_on_explicit`: bytes `0x90 0x3C 0x64` → `on_short(NOTE_ON, ch 0, 60, 100)`; `on_byte` saw all 3 bytes in order.
  - `note_off_and_vel0`: `0x80 0x3C 0x40` → NOTE_OFF; `0x90 0x3C 0x00` → NOTE_OFF (vel-0 convention).
  - `running_status`: `0x90 0x3C 0x64  0x3D 0x50` → two NOTE_ONs (60/100 then 61/80); `on_byte` saw 5 bytes.
  - `cc`: `0xB3 0x07 0x7F` → CC ch3 ctrl7 val127.
  - `program_change_2byte`: `0xC1 0x2A` → PROGRAM_CHANGE ch1 prog42.
  - `pitch_bend_14bit`: `0xE0 0x00 0x40` → PITCH_BEND d1=0, d2=64 (standard 14-bit value 8192 = d2*128+d1).
  - `aftertouch_and_poly`: `0xD0 0x5A` → AFTERTOUCH; `0xA0 0x3C 0x64` → POLY_AFTERTOUCH.
  - `sysex_whole`: `0xF0 0x7E 0x7F 0x09 0xF7` → one `on_sysex(4 bytes: 7E 7F 09 F7)` (F0 excluded, F7 included — store the body **including** F7? No: store everything between F0 and F7 **inclusive of F7**, exclusive of F0 — pin this: `data == {0x7E,0x7F,0x09,0xF7}`); **zero** `on_byte` calls for these 5 bytes.
  - `sysex_truncation`: F0 + 200×0x42 + F7 → `on_sysex` len == 127 and `on_sysex_truncated()` called once; framer back to normal after (next `0x90 0x00 0x40` still parses).
  - `realtime_interleaved`: `0x90 0x3C 0xF1 0x64` → F1 emitted via `on_byte`, NOTE_ON still completes.
  - `garbage_no_status`: leading `0x42 0x43` (data-only) → no shorts, no `on_byte`?? — pin: data-only bytes **before any status** are dropped entirely (not emitted), first real status starts cleanly.
  - `byte_fidelity`: feed a mixed 40-byte stream (explicit + running + 1 sysex + 1 realtime) → `on_byte` sequence == input minus sysex-interior bytes, exact order.
- [ ] **Step 2: Run to verify they fail**

Run: `cd extension && ./build.sh --macos && ctest --test-dir build/cmake-macos -R midi_framer --output-on-failure`
Expected: build FAILS (header missing) — or after creating empty stubs, tests FAIL.

- [ ] **Step 3: Implement `MidiFramer`** in `pd_midi_framer.cpp`
  State machine: `enum { NO_STATUS, STATUS_SEEN, IN_SYSEX }`; data-byte assembly from `status & 0xF0` (C0 and D0 = 1 data byte, F0 = sysex, all others incl. E0 = 2 data bytes — standard MIDI). `on_byte` emission point: for every non-sysex byte that the framer accepts (including status and data bytes of completed messages, including realtime). Keep the class allocation-free (fixed state only).
- [ ] **Step 4: Run tests until green**

Run: same as Step 2. Expected: PASS, all cases printed.
- [ ] **Step 5: Commit**

```bash
git add extension/src/core/pd_midi_framer.* extension/tests/midi_framer_tests.cpp extension/CMakeLists.txt
git commit -m "feat(midi): MidiFramer running-status parser (high-level + raw pass-through + sysex)"
```

---

### Task 2: Worker MIDI command execution + output hooks + per-instance output queue

**Files:**
- Modify: `extension/src/core/pd_command_queue.h` (opcodes + payload fields)
- Create: `extension/src/core/midi_output_queue.h` (header-only OK)
- Modify: `extension/src/libpd_worker.h`, `extension/src/libpd_worker.cpp`
- Test: `extension/tests/worker_midi_tests.cpp`
- Modify: `extension/CMakeLists.txt` (new test target)

**Interfaces:**
- Consumes: Task 1 `MidiShortMsg`/`MidiKind` (for the output-queue message type, redefined below as `MidiOutMsg`).
- Produces:
  - `PdCommand` new opcodes (existing ones untouched): `MIDI_NOTE = 6, MIDI_CC = 7, MIDI_PROGRAM_CHANGE = 8, MIDI_PITCH_BEND = 9, MIDI_AFTERTOUCH = 10, MIDI_POLY_AFTERTOUCH = 11, MIDI_BYTE = 12, MIDI_SYSEX = 13`; new fields `uint8_t midi[128] = {}; uint32_t midi_len = 0;` (SYSEX: `midi_len` bytes in `midi[]`, F0..F7 **inclusive**; NOTE/CC/etc.: `i32 = channel(0-15)`, `i64 = d1*256 + d2` packed — mirrors the existing packed style).
  - `struct MidiOutMsg { enum Kind : uint8_t { NOTE, CC, PROGRAM_CHANGE, PITCH_BEND, AFTERTOUCH, POLY_AFTERTOUCH, RAW_BYTE } kind; uint8_t channel, d1, d2, byte; };`
  - `class MidiOutputQueue { public: static constexpr int CAPACITY = 4096; void push(const MidiOutMsg &); int pop_all(std::vector<MidiOutMsg> &r_out); uint64_t dropped() const; }` — mutex + `std::deque`, drop-oldest on overflow (spec §4).
  - `LibpdWorker` additions: `MidiOutputQueue midi_out;` (public member) and static hook trampolines; the worker does **not** know PortMIDI.
- CMake: new executable `libpd_worker_midi_tests` = `tests/worker_midi_tests.cpp + src/libpd_worker.cpp + src/core/pd_command_queue.cpp + src/core/pd_audio_sink_generator.cpp` linking `libpd_static pthread godot-cpp` — **if linking godot-cpp into the test proves impractical (generator sink pulls Godot types),** exclude `pd_audio_sink_generator.cpp` and provide a trivial sink stub in the test file implementing the 3 `PdAudioSink` methods (the worker takes `PdAudioSink *sink = nullptr` — the test passes `nullptr`).

- [ ] **Step 1: Write the failing test** — `tests/worker_midi_tests.cpp`. One pd instance, **no DSP** (exercises the idle-instance path, Review Focus 3): `libpd_init()` (call_once OK), `libpd_new_instance()`, `libpd_set_instance`, `libpd_init_audio(0, 0, 44100)`; if that returns nonzero, `printf("SKIP: no audio for worker midi test\n"); return 0;` (skip-don't-fail). Load a temp patch file written to `mkstemp` at test start:
  ```
  #N canvas 0 0 300 200 12;
  #X obj 10 10 notein 1;
  #X obj 10 50 noteout 1;
  #X obj 100 10 ctlin 7;
  #X obj 100 50 ctlout 7;
  #X connect 0 0 1 0;
  #X connect 2 0 3 0;
  ```
  then `libpd_openfile`, `libpd_start_message(1.0f)` + `libpd_add_float(1.0f)` + `libpd_finish_message("pd", "dsp")` (same trick as the worker's INIT). Install the worker's hooks by calling the **same install function the worker uses** (expose `void install_midi_output_hooks(void *worker_ptr)` as a free function or worker static so the test drives the real code). Push commands and assert `worker.midi_out` contents:
  - `MIDI_NOTE` (ch0, 60, 100) → queue gets `NOTE {0,60,100}` (from `[noteout]` via the existing noteonhook path — it must now **also** land in `midi_out`, not only in the PdEvent ring).
  - `MIDI_CC` (ch3, ctrl7, 127) → `CC {3,7,127}`.
  - `MIDI_BYTE` 0x90 / 0x3C / 0x64 → `[notein]` receives the note → `NOTE` in queue (proves raw-byte input reaches high-level objects) and — because `[noteout]` re-emits — exactly the same NOTE (assert no extras).
  - `MIDI_SYSEX` (F0 7E 7F 09 F7) → no crash, queue unchanged (no `[sysexin]` in this patch).
  - Overflow: `MidiOutputQueue q; push 4097 times` → `q.dropped() == 1`, `pop_all` yields 4096 with the **newest** last.
- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && ./build.sh --macos && ctest --test-dir build/cmake-macos -R worker_midi --output-on-failure`
Expected: build FAILS (opcodes/queue missing).
- [ ] **Step 3: Implement**
  - `pd_command_queue.h`: opcodes + `midi[128]`/`midi_len` fields (zero-init keeps `sizeof(PdCommand)` growth bounded; queue is an unbounded `std::deque` — no capacity change needed).
  - `libpd_worker.cpp`: in `execute_command` add cases mapping each opcode to `libpd_noteon(channel, d1, d2)` / `libpd_controlchange` / `libpd_programchange` / `libpd_pitchbend(ch, d1 + d2*128)` / `libpd_aftertouch` / `libpd_polyaftertouch` / `libpd_midibyte(0, byte)` / per-byte `libpd_sysex(0, b)`; `pd_port = 0` everywhere. In the INIT path (next to the existing `libpd_set_printhook`/`libpd_set_noteonhook` block) install the remaining hooks: `controlchangehook, programchangehook, pitchbendhook, aftertouchhook, polyaftertouchhook, midibytehook` — C trampolines (same `c_noteonhook` pattern with `libpd_set_instancedata(this)`) that build a `MidiOutMsg` from the hook args (`channel & 15` decode) and `push` to `midi_out`; extend the existing `c_noteonhook` to push into `midi_out` as well as the existing ring.
- [ ] **Step 4: Run tests until green** (same command as Step 2).
- [ ] **Step 5: Commit**

```bash
git add extension/src/core/pd_command_queue.h extension/src/core/midi_output_queue.h extension/src/libpd_worker.* extension/tests/worker_midi_tests.cpp extension/CMakeLists.txt
git commit -m "feat(midi): worker MIDI command opcodes, pd output hooks, bounded output queue"
```

---

### Task 3: `MidiRoutingTable` — pure routing logic

**Files:**
- Create: `extension/src/core/midi_routing_table.h` (header-only)
- Test: `extension/tests/midi_routing_tests.cpp` (fold into Task 1's framer test executable or its own — its own, same CMake pattern)

**Interfaces:**
- Consumes: nothing.
- Produces (Task 4):
  - `class MidiRoutingTable { public: void add_route_in(int p_port, int64_t p_instance); bool remove_route_in(int, int64_t); std::vector<int64_t> instances_for_port(int p_port) const; void add_route_out(int64_t p_instance, int p_port); bool remove_route_out(int64_t, int); std::vector<int> ports_for_instance(int64_t p_instance) const; void forget_instance(int64_t p_instance); }`
  - `add_*` are idempotent; `remove_*` return false when absent; `forget_instance` clears both directions (spec §7 free-while-routed); internally mutex-guarded (MIDI I/O thread reads, main thread writes).

- [ ] **Step 1: Write the failing tests** — cases: fan-out to 3 instances + `instances_for_port` order-stable (sorted OK, pin: sorted); idempotent double-add → still one entry; `remove_route_in` false when absent; `forget_instance` empties both directions and leaves other instances intact; concurrent add/remove from 2 threads × 10k ops → no crash, final state consistent with a replay (or simply: after join, `forget` everything and check both lookups empty — pin the weaker check, the mutex is std::mutex).
- [ ] **Step 2: Run to verify it fails** (build error).
- [ ] **Step 3: Implement** (`std::mutex` + `std::unordered_set<std::pair<...>>` or two `unordered_map<int, unordered_set<...>>`).
- [ ] **Step 4: Run until green.**
- [ ] **Step 5: Commit** — `git commit -m "feat(midi): thread-safe fan-out routing table (pure unit)"` (add the 3 files).

---

### Task 4: `MidiRouter` — PortMIDI integration, I/O thread, output writer

**Files:**
- Create: `extension/src/midi_router.h`, `extension/src/midi_router.cpp`
- Modify: `extension/CMakeLists.txt` (add `src/midi_router.cpp` to `godot_libpd`; `PORTMIDI_ENABLED` definition when `BUILD_PORTMIDI`; framer/routing/output-queue sources already in the target or added)
- Test: `extension/tests/midi_writer_tests.cpp` (pure `MidiOutWriter`, same CMake pattern as Task 1)

**Interfaces:**
- Consumes: Tasks 1–3 (`MidiFramer`, `MidiShortMsg`, `MidiRoutingTable`, `MidiOutputQueue`, `PdCommand` MIDI opcodes).
- Produces (Task 5):
  - `class MidiRouter { public: MidiRouter(); ~MidiRouter(); bool available() const; std::vector<std::pair<int, std::string>> list_inputs() const; std::vector<std::pair<int, std::string>> list_outputs() const; int open_input(int p_pm_index); int open_output(int p_pm_index); void close_input(int p_port_id); void close_output(int p_port_id); void route_input(int p_port_id, int64_t p_instance, bool p_add); void route_output(int64_t p_instance, int p_port_id, bool p_add); void register_instance_output(int64_t p_instance, MidiOutputQueue *p_queue); void forget_instance(int64_t p_instance); void drain_signal_events(std::vector<MidiSignalEvent> &r_out); void shutdown(); std::function<void(int64_t, const PdCommand &)> on_midi_command; // MIDI I/O thread → main-thread-safe queue push };`
  - `struct MidiSignalEvent { bool is_sysex = false; int port_id = 0; MidiShortMsg msg; std::vector<uint8_t> sysex; };` — one struct, two shapes (non-sysex events ignore `sysex`, sysex events ignore `msg`).
  - `struct MidiOutWriter { std::vector<uint8_t> feed(uint8_t p_byte); };` — per-(instance,port) running-status writer: emits a stored status byte only when the next status differs; returns the bytes to hand to `PmWriteLong` (may be empty).
- Behavior pinned: PM input callback does **only** `PmReadLong` + copy (≤ 32-byte chunks, sysex captured whole via `PmIsLongMessage`) into the per-port ring; I/O loop polls ~1 ms; `Pm_OpenOutput` passes `"queue"`, 256; `PmTerminated` on any write → drop handle, unroute the port, notify (spec §7, Review Focus 1+3); close ops execute on the I/O thread via a control queue with 500 ms timeout (API side waits; router side just processes); all PM symbols behind `#ifdef PORTMIDI_ENABLED` with inert stubs otherwise (`available()` false). Decoded events go **only** into the internal signal ring (pulled by `drain_signal_events`) — one delivery mechanism, mirroring the existing `PdEventRing` pattern.

- [ ] **Step 1: Write the failing writer test** — `midi_writer_tests.cpp`: fresh writer + `0x90 0x3C 0x64` → emits `90 3C 64`; next `0x3D 0x50` (running status) → emits only `3D 50`; status change `0x91 0x00 0x40` → emits all 3; `feed` returns `std::vector<uint8_t>`.
- [ ] **Step 2: Run, verify fail** (build error).
- [ ] **Step 3: Implement `MidiOutWriter` + `MidiRouter`** per the pinned behavior. The router is the only file that includes `portmidi.h`. Fan-out per decoded event: for `instances_for_port(port)` (snapshot under the table's mutex) build the `PdCommand`s (high-level + `MIDI_BYTE` per raw byte + `MIDI_SYSEX`) and call `on_midi_command(instance_id, cmd)`; every decoded event is also appended to the internal signal ring.
- [ ] **Step 4: Run writer tests until green; then `./build.sh --macos` compiles the router** (no runtime PM test yet — that's Task 6).
- [ ] **Step 5: Commit** — `git commit -m "feat(midi): MidiRouter — PM ports, I/O thread, fan-out, bounded writes"` (add router + writer test + CMake).

**Reviewer gate:** Walk the `PmTerminated`/error paths (Review Focus 1), the close-on-I/O-thread ordering (spec §7), and that no `PmWrite*` appears outside the router.

---

### Task 5: `LibpdServer` MIDI API, signals, lifecycle

**Files:**
- Modify: `extension/src/libpd_server.h`, `extension/src/libpd_server.cpp`
- Modify: `extension/src/libpd_instance.h/.cpp` (expose worker output queue + command push; register/unregister with router)

**Interfaces:**
- Consumes: Task 4 `MidiRouter` (member of the server), Task 2 `MidiOutputQueue`/`PdCommand` opcodes.
- Produces (Task 6 test app): the exact GDScript API of spec §3 — methods `midi_available`, `midi_list_inputs`, `midi_list_outputs`, `midi_open_input`, `midi_open_output`, `midi_close_input`, `midi_close_output`, `midi_route_input`, `midi_route_output`; signals `midi_note_on(port_id, channel, pitch, velocity)`, `midi_note_off(...)`, `midi_cc(port_id, channel, controller, value)`, `midi_program_change(port_id, channel, program)`, `midi_pitch_bend(port_id, channel, value)`, `midi_aftertouch(port_id, channel, value)`, `midi_poly_aftertouch(port_id, channel, pitch, value)`, `midi_sysex(port_id, data)`, `midi_port_error(port_id, what)`.
- Wiring pinned: server constructor starts the router with `on_midi_command` = mutex-guarded lookup in a new `std::unordered_map<int64_t, LibpdInstance *>` (register/unregister from `_enter_tree`/`_exit_tree`, extended from the current id-only set) → `instance->push_midi_command(cmd)` (new `LibpdInstance` public method → `worker.push_command`); the server's `_process` calls `router.drain_signal_events(...)` (main thread) → `emit_signal`; instance register also calls `router.register_instance_output(id, &worker.midi_out)`, unregister calls `router.forget_instance(id)` (Review Focus 2: the map lookup and pointer use happen inside the mutex section's snapshot, command push immediately after — `LibpdInstance::push_midi_command` must tolerate the node being freed only via the normal `_exit_tree` → `forget_instance` order, which the server's unregister path guarantees).
- Android stubs: every method `push_error("MIDI not available on this platform"); return -1/false;` (`midi_available` → false); signals still declared (never fire).

- [ ] **Step 1: No unit test** (godot-cpp needs the engine; covered end-to-end by Task 6). Write the code, then the compile gate:
- [ ] **Step 2: `./build.sh --macos` builds clean** (this is the checkable result; a syntax-level mistake fails here).
- [ ] **Step 3: `./build.sh --android` (in the Android Docker image per `docs/android-build.md`) compiles with `BUILD_PORTMIDI=OFF`** — verifies the stub path. (If the Docker image isn't handy, at minimum `cmake -B build/cmake-android-configure-check ...` configure + build the two stub-gated files; record what was run.)
- [ ] **Step 4: Commit** — `git commit -m "feat(midi): LibpdServer MIDI API, typed signals, router lifecycle"` (server + instance files).

**Reviewer gate:** signal emissions only in `_process` (main thread); the instance-map mutex discipline (Review Focus 2); shutdown order per spec §7 (server destructor: stop routing → `router.shutdown()` → done; workers tear down independently).

---

### Task 6: Host integration — `--midi-smoke` on macOS (IAC loopback)

**Files:**
- Create: `test_project/scenes/test_midi.tscn`, `test_project/scripts/test_midi.gd`
- Modify: `test_project/project.godot` (add `test_midi.tscn` to `run/main_scene` only if that's the existing convention — otherwise just make it loadable; keep `test.tscn` the default)

**Interfaces:**
- Consumes: Task 5 GDScript API; existing `LibpdInstance.send_midi` (v1).
- Produces: a scene that is also the on-device test UI for Task 7 (same scene, two modes).

Scene contents (Label-based, spec §8.4):
- Port lists (inputs/outputs, `Text` labels — NOT RichTextLabel), Open In / Open Out buttons (index from a `SpinBox`), Route In → instance / Route Out ← instance buttons, a `Button` "Send test note" (`instance.send_midi(0, 60, 100)`), a live event **Label** fed by all 8 `midi_*` signals (ring buffer of 20 lines, one `label.text` rewrite per event — the pattern in `test_main.gd`), and a **MIDI-Learn demo**: a checkbox + two labels; when armed, the first `midi_cc` captured shows `controller`/`value`.
- One `LibpdInstance` node (child), `init()` + `load_patch("res://data/test_patch.pd")` (the existing `[notein 0 60] → print` patch — noteon ch0/pitch 60 prints via pd) + `start_dsp()`.
- `--midi-smoke` mode (same arg pattern as `test_main.gd`'s `--smoke`): find an IAC input + matching IAC output (names contain "IAC"); **if none: print `MIDI_SMOKE_SKIP no IAC bus` and `get_tree().quit(0)`** (Review Focus 5). Else: `midi_open_input(iac_in)`, `midi_open_output(iac_out)`, route both to the instance, `send_midi(0, 60, 100)`, then within 5 s wait for BOTH (a) the instance `print`ed the note (existing print-event signal) AND (b) a `midi_note_on` signal for pitch 60 arrived (IAC loopback: our output → IAC bus → our input). Either arriving proves one direction; both proves the loop. Success: `MIDI_SMOKE_OK`, quit 0; timeout: dump state, quit 1.

- [ ] **Step 1: Write the scene + script** (both modes in one script, `OS.get_cmdline_args()` check as in `test_main.gd`).
- [ ] **Step 2: Build the extension (`./build.sh --macos`) and run headless**

Run: `/Applications/Godot.app/Contents/MacOS/Godot --headless --path test_project res://scenes/test_midi.tscn -- --midi-smoke`
Expected: `MIDI_SMOKE_OK` (or the documented SKIP) with exit 0. If CoreMIDI needs no setup, IAC Bus 1 exists by default — if not, create one once via Audio MIDI Setup (record the exact steps taken in the commit message).
- [ ] **Step 3: Manual GUI check** — same command without `--headless`/args: open the scene, click Open In/Out on the IAC pair, Route both, hit "Send test note" → event Label shows the looped-back note, pd print line appears. (Screenshot not required; confirm on screen.)
- [ ] **Step 4: Commit** — `git commit -m "feat(midi): test_midi scene (IAC loopback smoke + MIDI-Learn demo UI)"` (scene, script, project.godot if touched).

---

### Task 7: Knulli on-device verification + packaging + docs

**Files:**
- Modify: `docs/knulli-build.md` (Audio section: add MIDI I/O; Input section untouched)
- Modify: `docs/superpowers/specs/2026-09-30-godot-libpd-midi-design.md` (status line → implemented)
- No engine changes — extension + app only.

**Interfaces:**
- Consumes: everything above; `engine_build/package-knulli.sh` (existing bundling).
- Produces: shipped bundle + documented procedure.

- [ ] **Step 1: Build the linux-arm64 extension in Docker** (Knulli image, `extension/build.sh --linux-arm64`), repackage the A133 bundle (`package-knulli.sh`), push to the device staging (`/tmp/godot-app` + exfat fallback while healthy — `docs/knulli-build.md` procedure), verify md5s.
- [ ] **Step 2: On-device run** (ES suspended; device is a testing instrument, not being played): launch `test_midi` from the staged bundle; in the app: list MIDI inputs — expect the ALSA sequencer virtual port(s); Open In on one, Open Out on another; from a **second** adb shell: `aconnect -l` to list the app's virtual ports, then connect with `aconnect <out-client>:<out-port> <in-client>:<in-port>`; press "Send test note" → the event Label must show the looped-back `note_on 60` and the pd print must fire. Then disconnect that connection with `aconnect -r <out-client>:<out-port>` from the second shell and confirm no further events + no hang. Power-cycle etiquette per `docs/knulli-build.md`.
- [ ] **Step 3: Update docs** — `docs/knulli-build.md` Audio section: "v2: MIDI I/O (PortMIDI, ALSA sequencer on A133 — `aconnect` loopback verified)" + the virtual-port test recipe; flip the spec status line.
- [ ] **Step 4: Commit** — `git commit -m "feat(midi): A133 on-device verification (ALSA sequencer loopback) + docs"`.

---

## Self-review notes (run at plan time, fixes applied inline)

- Spec §3 API → Tasks 5+6; §4 threads → Tasks 2/4/5 (invariants repeated in Global Constraints); §5 files → Tasks 1–5; §6 parsing → Task 1 (framewriter) + Task 4 (writer); §7 lifecycle → Tasks 3/4/5 + Review Focus; §8 tests → Task 1 (unit), 2 (worker), 6 (IAC), 7 (A133), 6 (scene); §9 risks → IAC-skip (Task 6 Step 2), queue sizing (Task 2: `PdCommandQueue` is an unbounded `std::deque` — the 130 KB worst-case worry from the spec is moot, no capacity change), PmWrite blocking (Task 4 Review Focus 3).
- One deliberate deviation from the spec's §5 file list: routing table and output queue got their own files (`core/midi_routing_table.h`, `core/midi_output_queue.h`) instead of living inside `midi_router.{h,cpp}` — spec's own "PM-independent units test without PortMIDI" requirement (spec §8.1) forces it.
- `PdCommand` carries `std::string` fields today, so the fixed `midi[128]` addition costs ≤ 130 B/slot in an unbounded deque — bounded by MIDI rate in practice.

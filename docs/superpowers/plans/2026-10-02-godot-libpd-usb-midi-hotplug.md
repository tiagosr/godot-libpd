# v2 M4 — USB-MIDI Hotplugging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect USB-MIDI (and any OS-enumerated MIDI) devices plugged/unplugged while running; keep `midi_list_inputs/outputs` live; fire `midi_port_added`/`midi_port_removed`; auto-close dead real-device ports.

**Architecture:** Router-side periodic re-enumeration + diff inside the existing MIDI I/O thread (Approach 1). The diff keys by `(direction, name)` (indices are volatile), auto-closes only `real_device` open ports on a *confirmed* removal (2-consecutive-empty debounce guards transient-empty `list_ports()`), and reports via an `on_port_changed` callback that the server turns into the two signals. No `MidiBackend` interface change.

**Tech Stack:** godot-cpp GDExtension, C++17, RtMidi (CoreMIDI/ALSA/AMIDI) + PortMIDI fallback, CMake/ctest.

**Spec:** `docs/superpowers/specs/2026-10-02-godot-libpd-usb-midi-hotplug-design.md`

> **STATUS: MILESTONE COMPLETE (2026-10-02).** All five tasks done and
> verified: router diff (ctest 10/10 RtMidi + 11/11 PortMIDI), refresh +
> server API, and on-device hotplug on both devices (A133 RtMidi ALSA
> `HOTPLUG_SMOKE_OK added=1 removed=1`; Android RtMidi `added=1 removed=0`
> via the in-process loopback). The auto-close-on-external-removal path was
> additionally verified on **real hardware on all three backends** — macOS
> (CoreMIDI), A133 (ALSA, USB OTG), and RG DS (Android AMIDI, USB OTG) —
> physical nanoKONTROL2 + FM-1 plug/unplug → `midi_port_removed` + `device
> removed`.
> Spec → implemented; SDD ledger closed.

## Global Constraints

- All `libpd_*` on instance worker threads; **all backend port ops on the MIDI I/O thread**; Godot signals on the main thread only. (Inherited, spec §5.)
- `MidiBackend::list_ports()` is const + self-locked; the router may call it from the I/O thread.
- The `on_port_changed` / `on_port_error` callbacks fire on the I/O thread and **must never call any `MidiRouter` method** (re-entrancy contract, `midi_router.h`).
- Port records and diff state (`seen_`, `last_index_`, `pending_empty_`, `non_real_indices_`) are mutated **only** on the I/O thread. `pending_port_events_` is guarded by `midi_mutex`.
- Default poll interval `0.5 s` (atomic, settable). First re-enumeration is immediate (`last_reeum_` starts at `time_point_min`) → **annotated-on-start** baseline.
- **No auto-reopen** on reconnect (re-plug fires `midi_port_added` with a fresh index; the app re-opens).
- Submodules unchanged; `MidiBackend` interface unchanged.
- Subagent timeouts must be 7200000 ms (slow model endpoint). HARD VERIFICATION RULE: every on-device claim needs a real command + pasted output.
- A133 gotchas: engine loads `<basename>.pck`; stdout-to-file is block-buffered (use `stdbuf -oL -eL`); `uikeys` must exist before app launch; injection tests run with ES suspended. Android: C-level `printf` is NOT in logcat (only GDScript `print()`); `.so` must be refreshed into gradle assets before `gradlew`.

## Review Focus

- **Transient empty `list_ports()`** (bad CoreMIDI/ALSA/AMIDI probe tick) → must NOT auto-close live ports (debounce skips one empty tick) — Task 1 test 7.
- **Only device unplugged** (genuine empty list) → must STILL fire `midi_port_removed` + auto-close (debounce confirms on the 2nd empty tick) — Task 1 test 7.
- **Index shift on removal** (a surviving device re-indexes) → the survivor must NOT be spuriously removed (name-keyed diff, not index-keyed) — Task 1 test 4.
- **Virtual/loopback ports** (non-`real_device`) disappear from the list → `midi_port_removed` fires but they are NOT auto-closed; a `real_device` port with the same name IS — Task 1 test 8.
- **`midi_port_*` signal arguments** carry the correct `kind` ("input"/"output"), the current index for add, and the *remembered* last index for remove — Task 1 tests 2/3 + Task 3 host smoke.

---

### Task 1: Router hotplug re-enumeration + diff engine (COMPLETE)

**Files:**
- Modify: `extension/src/midi_router.h` (add `on_port_changed`, `set_poll_interval`, `Port.real_device`+`device_name`, `PortKey`, diff state members, `REFRESH`-adjacent scaffolding, `reenum_and_diff`).
- Modify: `extension/src/midi_router.cpp` (implement diff in `io_loop`; set `real_device`/`device_name`/`non_real_indices_` in the open paths).
- Test: `extension/tests/midi_backend_fake_tests.cpp` (extend `FakeBackend`; add hotplug tests 1–8).

**Interfaces:**
- Consumes: `MidiBackend::list_ports()`, `MidiBackend::PortHandle`, existing `Port`, `close_port(int)`, `notify_port_error(int, const char*)`, `io_loop()`, the control-queue `open_port(bool,int)`.
- Produces (for Task 2/3):
  - `std::function<void(bool p_added, const char *p_kind, int p_index, const char *p_name)> MidiRouter::on_port_changed;` (public; fires on the I/O thread; `p_kind` is `"input"`/`"output"`).
  - `void MidiRouter::set_poll_interval(double p_seconds);` (public, thread-safe).
  - `int MidiRouter::reenum_and_diff();` (private, I/O thread only; returns changed-port count). Task 2's `REFRESH` calls it.
  - (Task 2 adds `refresh_ports()` + the `REFRESH` control op on top of `reenum_and_diff()`.)

- [ ] **Step 1: Extend `FakeBackend` for hotplug**

In `extension/tests/midi_backend_fake_tests.cpp`, generalize `FakeBackend::list_ports()` to return a mutable system device set plus the loopback ports, and add test helpers. Keep the existing `kFakeDevice` (index 0, "Fake Device", in+out) as the default seeded device so prior tests still pass.

- Add `std::vector<MidiBackendPort> devices_;` (seeded in the constructor with the single `kFakeDevice` port) and have `list_ports()` return `devices_` followed by `loopback_ports_`.
- Generalize `open_input`/`open_output` to accept **any** index present in `devices_` with a matching side (not just `kFakeDevice`), recording the handle as today.
- Add helpers: `void set_devices(std::vector<MidiBackendPort> p_devs)` (replace `devices_` under `mutex_`) and `void add_device(int p_index, const std::string &p_name, bool p_in, bool p_out)`.
- Add a way to observe diff results: the test will bind `router.on_port_changed` (Task 1 Step 3) to a mutex-guarded `std::vector<std::tuple<bool,std::string,int,std::string>> port_events`.

- [ ] **Step 2: Write the failing hotplug tests**

Add to `main()` in `midi_backend_fake_tests.cpp` (after the existing sections), using a fresh `MidiRouter` over a fresh `FakeBackend` (or reusing the existing one with a reset device set). Bind `router.on_port_changed` to record events. Set a short poll interval first: `router.set_poll_interval(0.02);`.

Write these tests (each `CHECK`s the asserted condition; use the existing `wait_for(...)` helper):

1. **Startup annotation** — with `devices_ = {A(in), B(out)}` and poll interval 0.02 s: within ~500 ms, `port_added` fires for **both** `(input, A)` and `(output, B)` (exactly one each).
2. **Add diff** — start with `devices_ = {A}` (let it annotate), then `fake->add_device(2, "C", true, false)`; expect exactly one new `port_added(input, index 2, "C")`; no `port_removed`.
3. **Remove diff + auto-close** — open input on device A (`router.open_input(0)` → `in_port`), route it, then `fake->set_devices({})` for **two** 0.02-s ticks (so the debounce confirms); expect `port_removed(input, ..., "Fake Device")` **and** the backend reports the input no longer open (`fake->open_inputs()` no longer contains the opened handle) and a `midi_port_error`-equivalent fired (assert via a bound `router.on_port_error` capturing `port_id == in_port`, `"device removed"`).
4. **Index shift** — `devices_ = {A(0), C(1)}`; open input on C (index 1). Remove A (so C re-indexes to 0): `fake->set_devices({{0,"C",true,false}})` for two ticks. Expect **no** `port_removed` for C (the name-keyed diff sees C still present), and C is **not** closed (`fake->open_inputs()` still contains its handle).
5. **Poll interval** — (covered by tests 1–4 using 0.02 s; no separate assertion needed beyond "events landed within the wait window").
6. **Transient-empty blip** — `devices_ = {A}`; open input on A. Then `fake->set_devices({})` for exactly ONE 0.02-s tick, then `fake->set_devices({{0,"Fake Device",true,true}})`; assert **no** `port_removed` and A's port is **still open** (the blip was ignored).
7. **Confirm empty (last device)** — `devices_ = {A}`; open input on A. `fake->set_devices({})` and hold for **two** 0.02-s ticks; assert `port_removed` fired **and** A's port is closed. (This is the genuine-unplug case; distinct from test 6's blip.)
8. **Virtual/loopback not auto-closed** — create a loopback (`router.create_virtual_loopback("LB")`), open its input via `router.open_input(kLoopbackIn)`. The loopback index is `non_real`. Remove everything *except* keep the loopback listed: simulate by `fake->set_devices({})` (the loopback stays in `list_ports()` because `loopback_ports_` is separate) for two ticks → assert the loopback input port is **not** closed (still in `fake->open_inputs()`) while any real-device port matching a removed key **was** closed.

- [ ] **Step 3: Implement the router diff**

In `extension/src/midi_router.h`:
- Add `struct PortKey { bool is_input; std::string name; bool operator<(const PortKey &o) const; };` (order by `is_input` then `name`).
- Add to `Port`: `bool real_device = false;` and `std::string device_name;`.
- Add members (I/O-thread-only unless noted): `std::chrono::steady_clock::time_point last_reeum_{std::chrono::steady_clock::time_point::min()};`, `std::atomic<double> poll_interval_{0.5};`, `std::set<PortKey> seen_;`, `std::map<PortKey,int> last_index_;`, `bool pending_empty_ = false;`, `std::set<int> non_real_indices_;`.
- Add public: `std::function<void(bool, const char *, int, const char *)> on_port_changed;`, `void set_poll_interval(double);`.

- Add private `int reenum_and_diff();` + `void close_real_device_ports(bool p_is_input, const std::string &p_name);`.

In `extension/src/midi_router.cpp`:
- `set_poll_interval`: `poll_interval_.store(p_seconds);`.
- In `open_port(bool,int)`: after a successful open, set `port->device_name` by scanning `backend_->list_ports()` for the matching `(index, side)`; set `port->real_device = non_real_indices_.count(p_device_index) == 0;`.
- In the `OPEN_VIRTUAL_*` handling (where `create_virtual_*` returns `device_index`): `non_real_indices_.insert(device_index);` before `open_port`.
- In the `CREATE_LOOPBACK` handling: snapshot `backend_->list_ports()` before `backend_->create_virtual_loopback(name)`; after, insert every index present after but not before into `non_real_indices_`.
- `reenum_and_diff()` (I/O thread): per spec §4.1 — build `current` from `backend_->list_ports()`; apply the **2-consecutive-empty debounce**; fire `on_port_changed(true/false, kind, index, name)` for adds/removals (index = current for add, `last_index_` for remove); on each removed key call `close_real_device_ports(is_input, name)`; update `seen_`/`last_index_`; return changed count. `kind` is `"input"` when `is_input` else `"output"`.
- `close_real_device_ports(is_input, name)`: under `mutex_`, collect `port_id`s where `in_use && is_input==direction && real_device && device_name==name`; then for each, `close_port(pid)` and `notify_port_error(pid, "device removed")`.
- In `io_loop()`, after `output_stage()` and before the 1 ms sleep: `if (steady_clock::now() - last_reeum_ >= milliseconds((long)(poll_interval_.load()*1000.0))) { reenum_and_diff(); last_reeum_ = now; }`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build extension/build/cmake-macos && ctest --test-dir extension/build/cmake-macos --output-on-failure`
Expected: the new hotplug tests pass; the full suite is green (previously 10/10, now 10/10 with the extended fake tests in the same binary — confirm `midi_backend_fake_tests` passes and no regression in the other 9).

- [ ] **Step 5: Commit**

```bash
git add extension/src/midi_router.h extension/src/midi_router.cpp extension/tests/midi_backend_fake_tests.cpp
git commit -m "M4 Task 1: router hotplug re-enumeration + diff (name-keyed, debounced)

I/O-thread periodic list_ports() diff keyed by (direction,name); fires
on_port_changed; auto-closes real_device ports on confirmed removal
(2-consecutive-empty debounce guards transient empty enumeration);
non_real_indices_ marks virtual/loopback so they are never auto-closed;
set_poll_interval + immediate first tick (annotated-on-start)."
```

---

### Task 2: Router `refresh_ports()` + REFRESH control op (COMPLETE)

**Files:**
- Modify: `extension/src/midi_router.h` (add `REFRESH` to `ControlOpType`; declare + implement `refresh_ports()`).
- Modify: `extension/src/midi_router.cpp` (handle `REFRESH` in `process_control_ops`).
- Test: `extension/tests/midi_backend_fake_tests.cpp` (test 5: refresh returns change count).

**Interfaces:**
- Consumes: `reenum_and_diff()` (Task 1), the control queue (`enqueue_control`, `process_control_ops`).
- Produces: `int MidiRouter::refresh_ports();` — thread-safe; enqueues a `REFRESH` op, waits ≤ 500 ms, returns the diff's changed-port count; returns -1 if there is no I/O thread or the wait times out (timeout surfaces via `notify_port_error(-1, ...)`).

- [ ] **Step 1: Write the failing test**

In `midi_backend_fake_tests.cpp`: with a short poll interval, start from a known device set, call `int n = router.refresh_ports();` immediately after a `fake->add_device(...)` that has not yet been seen by a periodic tick; assert `n == 1` (one added) and that the corresponding `port_added` fired. Also assert `router.refresh_ports() == 0` when nothing changed.

- [ ] **Step 2: Implement `refresh_ports()`**

In `midi_router.cpp`: `refresh_ports()` enqueues `ControlOpType::REFRESH` (no index/name) and waits ≤ `kControlTimeoutMs`; on the I/O thread, `process_control_ops()` handles `REFRESH` by calling `reenum_and_diff()` and fulfilling the op's promise with the returned count. On the API side, a timeout calls `notify_port_error(-1, "midi refresh timed out")` and returns -1.

- [ ] **Step 3: Run tests to verify pass**

Run: `cmake --build extension/build/cmake-macos && ctest --test-dir extension/build/cmake-macos --output-on-failure`
Expected: refresh test passes; full suite green.

- [ ] **Step 4: Commit**

```bash
git add extension/src/midi_router.h extension/src/midi_router.cpp extension/tests/midi_backend_fake_tests.cpp
git commit -m "M4 Task 2: router refresh_ports() + REFRESH control op

Forces an immediate reenum_and_diff on the I/O thread; returns the
changed-port count; timeout -> midi_port_error(-1)."
```

---

### Task 3: Server GDScript API + host smoke (COMPLETE)

**Files:**
- Modify: `extension/src/libpd_server.h` (two signals, `PortEvent`, `pending_port_events_`, `midi_refresh_ports()`, `midi_set_poll_interval`/`midi_get_poll_interval` + `midi_port_poll_interval` property, amended list-method docs).
- Modify: `extension/src/libpd_server.cpp` (bind methods/signals, wire `on_port_changed`, emit in `_drain_midi_events`, implement the new methods).
- Modify: `extension/src/midi_backend.h` (doc-only note on index stability).
- Test/scene: `test_project/scripts/test_midi.gd` + `test_project/scenes/test_midi.tscn` (a `--midi-hotplug-smoke` CLI mode); optionally `test_project/project.godot` (run arg).
- Test: `extension/tests/midi_backend_fake_tests.cpp` already covers the router; the server is verified by compile + the host smoke below.

**Interfaces:**
- Consumes: `MidiRouter::on_port_changed`, `MidiRouter::refresh_ports()`, `MidiRouter::set_poll_interval`, the existing `pending_port_errors_`/`midi_mutex`/`_drain_midi_events()` pattern.
- Produces (GDScript-facing): signals `midi_port_added(kind: String, index: int, name: String)`, `midi_port_removed(kind: String, index: int, name: String)`; methods `int midi_refresh_ports()`, `void midi_set_poll_interval(float s)`, `float midi_get_poll_interval()`; property `midi_port_poll_interval: float` (default 0.5).

- [ ] **Step 1: Add the signals + wiring in `libpd_server.h`/`.cpp`**

- In `_bind_methods()`: `ADD_SIGNAL(MethodInfo("midi_port_added", ...))` and `ADD_SIGNAL(MethodInfo("midi_port_removed", ...))` with args `(String kind, int index, String name)`; `ClassDB::bind_method(D_METHOD("midi_refresh_ports"), &LibpdServer::midi_refresh_ports);`, `midi_set_poll_interval`, `midi_get_poll_interval`; add the `midi_port_poll_interval` `PROPERTY(...)` (float, default 0.5) with a setter that forwards to `midi_router.set_poll_interval`.
- Add `struct PortEvent { bool added; String kind; int index; String name; };` and `std::vector<PortEvent> pending_port_events_;` (guarded by `midi_mutex`).
- In the constructor, wire: `midi_router.on_port_changed = [this](bool added, const char *kind, int index, const char *name){ std::lock_guard<std::mutex> l(midi_mutex); pending_port_events_.push_back({added, String(kind), index, String(name)}); };`
- In `_drain_midi_events()`, after the `pending_port_errors_` block: swap out `pending_port_events_` and, per event, `emit_signal(added ? "midi_port_added" : "midi_port_removed", kind, index, name);`.

- [ ] **Step 2: Implement `midi_refresh_ports()` / poll-interval methods**

`int LibpdServer::midi_refresh_ports() { return midi_available() ? midi_router.refresh_ports() : -1; }`. `midi_set_poll_interval(float s){ midi_router.set_poll_interval(s); }` (also store the member backing the property). `float midi_get_poll_interval() { return (float)poll_interval member; }`.

- [ ] **Step 3: Add the `midi_backend.h` index-stability doc note**

In the `*port index*` convention comment, amend: indices are stable **only while the device set is unchanged**; re-query `list_ports()` after a hotplug add/remove.

- [ ] **Step 4: Add the `--midi-hotplug-smoke` device-free host smoke**

In `test_midi.gd`, add a mode gated on the CLI arg `--midi-hotplug-smoke` (read from `OS.get_cmdline_user_args()` or `OS.get_cmdline_args()`), that:
1. Connects to `Libpd.server.midi_port_added` / `midi_port_removed`, recording into arrays.
2. Sets `Libpd.server.midi_set_poll_interval(0.05)`.
3. `await` ~200 ms to let the annotated-on-start baseline fire (record which ports were announced).
4. Opens a virtual input (`var p = Libpd.server.midi_open_virtual_input()`), `await` up to ~1 s for a `midi_port_added` whose name matches the virtual-in name.
5. Closes it (`Libpd.server.midi_close_input(p)`), `await` up to ~1 s for a `midi_port_removed` with that name.
6. Prints `HOTPLUG_SMOKE_OK added=<n> removed=<n>` (or `HOTPLUG_SMOKE_FAIL <reason>`) and `get_tree().quit(failures?1:0)`.

This is **device-free** (virtual ports appear/disappear in `list_ports()` on CoreMIDI/ALSA), so it runs on macOS headless and on-device. (On a backend where virtual ports do not enumerate — none in our builds do — the smoke would fail; document that.)

- [ ] **Step 5: Build all backends + run the host smoke**

- `cmake --build extension/build/cmake-macos && ctest --test-dir extension/build/cmake-macos --output-on-failure` (full suite green).
- Re-export the PCK with `run/main_scene` switched to `test_midi.tscn` and `extra_args` including `--midi-hotplug-smoke` (temp switch; restore after). Run the macOS `test.app` and confirm `HOTPLUG_SMOKE_OK added>=1 removed>=1` on stdout, exit 0.
- PortMIDI fallback build: `cmake -DMIDI_BACKEND=portmidi ...` (or the build.sh equivalent), build + `ctest` green + the same smoke prints `HOTPLUG_SMOKE_OK`.

- [ ] **Step 6: Revert the temp switches + commit**

Restore `project.godot` / `export_presets.cfg` / gradle assets to their committed state (as done in M3 Task 2). Then:

```bash
git add extension/src/libpd_server.h extension/src/libpd_server.cpp extension/src/midi_backend.h test_project/scripts/test_midi.gd test_project/scenes/test_midi.tscn
git commit -m "M4 Task 3: server GDScript API + device-free host hotplug smoke

midi_port_added/midi_port_removed signals, midi_refresh_ports(),
midi_port_poll_interval property; on_port_changed -> main-thread
emission; --midi-hotplug-smoke uses virtual ports (device-free) to
exercise the add + remove paths."
```

---

### Task 4: On-device verification (A133 ALSA + Android RG DS) + PortMIDI regression (COMPLETE)

**Files:** none in-repo (verification only; may touch `test_project` scene wiring if a device-specific knob is needed). Update the SDD ledger + plan checkboxes.

**Interfaces:** Consumes the shipped extension (Task 1–3). Produces verification evidence.

- [x] **Step 1: A133 (ALSA) on-device** — verified 2026-10-02: `HOTPLUG_SMOKE_OK added=1 removed=1` (RtMidi ALSA, virtual port add+remove diff; ALSA reports full `client:port` name → smoke matches substring); baseline `SMOKE_OK` green; aconnect shows real `128:0` port + clean 4-client teardown. Auto-close-on-external-removal verified on real hardware on ALL THREE backends — macOS (CoreMIDI), A133 (ALSA, USB OTG), and RG DS (Android AMIDI, USB OTG) — nanoKONTROL2 + FM-1 unplug → `device removed`. The RG DS test surfaced + fixed three Android-only USB-open bugs in vendored RtMidi (see ledger).

Stage the M4 `.so` + a `test_midi` PCK to `/tmp/godot-app/` (tmpfs; ES suspended). With `--midi-hotplug-smoke` (virtual ports) **and** a manual `aconnect`-visible device scenario:
- Confirm `HOTPLUG_SMOKE_OK` on stdout (via `stdbuf -oL -eL`).
- Manually: `aconnect -l` before/after creating an ALSA virtual (or a synthetic seq client) → observe `midi_port_added`/`midi_port_removed` lines on stdout; open the port, then remove the device → confirm the auto-close (`midi_port_error "device removed"`) and no further delivery.
- Baseline `SMOKE_OK` (audio) still green.

- [x] **Step 2: Android (RG DS) on-device** — verified 2026-10-02: `HOTPLUG_SMOKE_OK added=1 removed=0` (RtMidi Android; no AMIDI virtual-port API → device-free vehicle is the in-process loopback, which enumerates → `midi_port_added`; loopback has no runtime close so removed=0 on-device, removed path ctest-covered). No FATAL/SIGSEGV, clean exit. (No USB-MIDI device for the real-unplug leg.)

Rebuild the Android `.so` (clean), refresh it into `test_project/android/build/libs/...`, rebuild assets, `gradlew assembleStandardDebug`, `apksigner sign`, `install -r`. With the temp `--midi-hotplug-smoke` switch:
- logcat (GDScript `print()`) shows `HOTPLUG_SMOKE_OK added>=1 removed>=1`; `grep -icE "FATAL|AndroidRuntime: F|SIGSEGV|SIGABRT|libc : Fatal"` == 0; clean exit.
- (Real-device unplug, if a USB MIDI device is available: plug/unplug → `midi_port_added`/`removed` in logcat.)

- [ ] **Step 3: PortMIDI fallback on-device (A133)** — SKIPPED (optional): the hotplug diff is backend-agnostic (router) and already verified on RtMidi(ALSA) on-device + RtMidi/PortMIDI on host (Task 3). PortMIDI+ALSA on-device would be redundant given the backend-agnostic diff; revisit only if a PortMIDI-specific regression is suspected.

- [x] **Step 4: Revert temp switches, update ledger, commit plan checkboxes**

Revert `project.godot`/`export_presets.cfg`/gradle assets. Append the on-device evidence to `.superpowers/sdd/2026-10-02-godot-libpd-usb-midi-hotplug/progress.md`. Commit the plan-file checkbox updates.

---

### Task 5: Docs + closure (COMPLETE)

**Files:**
- Modify: `extension/README.md` (MIDI backends section → add hotplug; new "USB-MIDI hotplugging (M4)" section: signals, refresh, poll interval, debounce, non-goals).
- Modify: `docs/knulli-build.md` (MIDI section → hotplug: ALSA virtual/synthetic device add/remove, aconnect, the on-device gotchas).
- Modify: `docs/android-build.md` (hotplug: AMIDI device callback, logcat visibility, no-auto-reopen).
- Modify: `docs/superpowers/specs/2026-10-02-godot-libpd-usb-midi-hotplug-design.md` (status → implemented).
- Modify: `docs/superpowers/plans/2026-10-02-godot-libpd-usb-midi-hotplug.md` (Task N → COMPLETE as done).
- Modify: `.superpowers/sdd/2026-10-02-godot-libpd-usb-midi-hotplug/progress.md` (final closure entry).

**Interfaces:** Consumes the finished feature. Produces docs + closed milestone.

- [ ] **Step 1: Update `extension/README.md`** — document `midi_port_added`/`midi_port_removed`, `midi_refresh_ports()`, `midi_port_poll_interval`, the name-keyed + debounce behavior, index-stability caveat, and non-goals (no auto-reopen, no stable tokens).
- [ ] **Step 2: Update `docs/knulli-build.md`** — hotplug recipe on A133 (ALSA virtual/synthetic device, `aconnect`, stdout gotchas incl. `stdbuf` + `<basename>.pck` + `uikeys`).
- [ ] **Step 3: Update `docs/android-build.md`** — hotplug on Android (AMIDI `getDevices` per poll, logcat-only GDScript `print`, no-auto-reopen).
- [ ] **Step 4: Flip spec → implemented; mark plan Tasks COMPLETE; close the SDD ledger.**
- [ ] **Step 5: Commit**

```bash
git add extension/README.md docs/knulli-build.md docs/android-build.md \
  docs/superpowers/specs/2026-10-02-godot-libpd-usb-midi-hotplug-design.md \
  docs/superpowers/plans/2026-10-02-godot-libpd-usb-midi-hotplug.md \
  .superpowers/sdd/2026-10-02-godot-libpd-usb-midi-hotplug/progress.md
git commit -m "M4 Task 5: docs + closure — USB-MIDI hotplugging (M4) complete"
```

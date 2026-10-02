# v2 M4 — USB-MIDI Hotplugging (live device add/remove)

**Status:** proposed (2026-10-02)
**Depends on:** v2 M1 (MIDI I/O + `MidiBackend`), M2 (RtMidi Android), M3 (RtMidi
host). Builds on the existing server-wide `MidiRouter` + `MidiBackend` seam; adds
no new backend interface surface.

## 1. Goal

Detect USB-MIDI (and any OS-enumerated MIDI) devices being **plugged in or
unplugged while the app is running** and reflect that live in
`LibpdServer.midi_list_inputs()` / `midi_list_outputs()`, without a restart.

Concretely:

- The device list stays live; changes are announced by new
  `midi_port_added` / `midi_port_removed` signals.
- An **open** input/output port whose underlying device leaves is
  **auto-closed** and surfaced via the existing `midi_port_error` signal — no
  silent dead handle.
- Convenience: `midi_refresh_ports()` forces re-enumeration on demand;
  `midi_port_poll_interval` tunes the polling cadence.
- Works on **every** backend we build: macOS CoreMIDI, Linux ALSA, Android AMIDI,
  and the PortMIDI fallback (which gets it for free from the same router diff).

**Out of scope / non-goals (deferred):**
- Auto-reopen of a device on reconnect (the app re-opens it; see §6).
- Stable device tokens (port indices shift on device change — §4 documents the
  contract; we key the diff by name, not index).
- Auto-open / auto-route of newly-appearing devices (they are *openable*; the
  user opens + routes).
- Name-collision disambiguation (rare; documented).
- Per-port / per-device polling intervals.

## 2. Technical basis (verified against the vendored backends)

**All backends re-report the live device set on every `list_ports()` call, but
none exposes a public "port changed" push callback.** So the mechanism is
**router-side periodic re-enumeration + diff**, reusing the existing
`MidiBackend::list_ports()`:

- **CoreMIDI** (`MidiInCore::getPortCount`): re-queries `MIDIGetNumberOfSources()`
  every call; `getPortName` looks up the endpoint name. Non-blocking
  `CFRunLoopRunInMode(..., 0, false)` spin.
- **ALSA** (`MidiInAlsa`): a built-in monitor thread keeps RtMidi's list
  current; `getPortCount`/`getPortName` read it.
- **AMIDI** (`MidiInAndroid::getPortCount`): calls `connect()` →
  `androidRefreshMidiDevices()` → `MidiManager.getDevices()` — a fresh system
  query per call (heavier: JNI per call).

Two consequences that drive the design:

1. **Port indices are NOT stable across device changes on any backend** — a
   removal shifts enumeration order. The existing interface doc
   ("indices stable for process lifetime") is therefore amended: indices are
   stable **only while the device set is unchanged**. The diff and the signals
   key by **(direction, port name)**; the index is carried as transient metadata
   and consumers must re-query after an add/remove.
2. **A dead open port is NOT reported by `poll()`** on the host backends —
   `poll_input` returns `OK` with no words (never `FATAL`) when its device is
   unplugged (there is no device-removal signal on those APIs). So the diff is
   also the *only* mechanism for detecting "this open port's device left" and
   closing it.

## 3. Approach

**Approach 1 (chosen): the MIDI I/O thread performs the re-enumeration + diff**,
time-gated inside the existing `MidiRouter::io_loop()`.

- The I/O thread already owns every backend handle and does all open/close;
  closing a dead open port on device-removal therefore happens under the **same
  thread ownership** as every other port op (zero new races).
- Add/remove changes are reported via a new router callback
  `on_port_changed` (fires on the I/O thread, exactly like the existing
  `on_port_error`); the server copies them into a main-thread ring and emits
  the signals in `LibpdServer::_drain_midi_events()` (never from the I/O
  thread).
- Rejected alternatives: (2) a separate monitor thread (extra threads +
  cross-thread sync for a cheap periodic poll); (3) main-thread re-enumeration
  (dead-port close still needs a control-queue round-trip; adds main-thread
  load).

## 4. Design

### 4.1 Router — loop integration (`midi_router.{h,cpp}`)

A time-gated step in `io_loop()`, after `output_stage()` and before the 1ms
sleep:

- New members:
  - `last_reeum_` — `steady_clock` time point (starts at `time_point_min`).
  - `poll_interval_` — atomic seconds (default `0.5`).
  - `seen_` — set of `PortKey { bool is_input; std::string name; }`.
  - `last_index_` — `map<PortKey, int>` (remembered per key for the remove
    signal; index is volatile).
- **First tick**: `last_reeum_` is at minimum → re-enumerate immediately. Because
  `seen_` starts empty, **everything present emits `port_added`** — this is the
  *annotated-on-start* behavior (see §4.4).
- **Each subsequent due tick** (`now − last_reeum_ ≥ poll_interval_`):
  - `ports = backend_->list_ports();`
  - Build the current set `{ (direction, name) → index }`.
  - `added = current − seen_` → fire `on_port_changed(true, kind, index, name)`
    per entry (current index).
  - `removed = seen_ − current` → for each:
    - fire `on_port_changed(false, kind, remembered_index, name)`;
    - **close every open router port flagged `real_device`** whose
      `(direction, name)` matches → auto-unroute + backend close +
      `notify_port_error(port_id, "device removed")`.
  - update `seen_` / `last_index_`; `last_reeum_ = now`.
- `backend_->list_ports()` is const and self-locked (already called from the
  API thread by `list_inputs`); calling it from the I/O thread is safe.

**Transient-empty guard (debounce).** A backend `list_ports()` can transiently
return an **empty** list (a bad probe tick on CoreMIDI/ALSA/AMIDI). A naive
diff would read that as "every device removed" and auto-close every open
port. But the *only device unplugged* case is also a genuine empty list — so a
plain "skip empty" guard would miss a real unplug. The diff therefore uses a
**2-consecutive-empty debounce**:

- New list **non-empty** → apply the diff normally (adds + removals), update
  `seen_`, clear `pending_empty_`.
- New list **empty**:
  - `seen_` empty → nothing to do.
  - `seen_` non-empty and `pending_empty_ == false` → set
    `pending_empty_ = true`, **skip** (wait for confirmation).
  - `seen_` non-empty and `pending_empty_ == true` → **confirm** empty: apply
    removals (all removed), set `seen_` empty, clear `pending_empty_`.

A transient blip (recovers within one tick) is ignored; a real unplug
(persists to the next tick, ~one `poll_interval` later) is applied.

### 4.2 Router — `Port` record

Add to `Port`:

- `bool real_device = false;` — true only for ports backed by an OS/device
  endpoint that can hot-plug. Set in `open_port` from a router-maintained
  `std::set<int> non_real_indices_` (I/O-thread-only): a port is
  `real_device` iff its open index is **not** in that set. The set is populated
  by the only producers of non-real indices, both on the I/O thread:
  - `OPEN_VIRTUAL_*`: the `create_virtual_*` `device_index` is inserted.
  - `CREATE_LOOPBACK`: snapshot `list_ports()` before/after
    `backend_->create_virtual_loopback(name)`; the new indices are inserted.
  So `open_input`/`open_output` on a **real** device index → `real_device=true`;
  the same calls on a loopback index, or `open_virtual_*`, → `false`. Only
  `real_device` ports are auto-closed by the diff (virtual/loopback are
  app-owned and never closed by it). `close_port` is idempotent, so even a
  stray close of an already-closed port is a safe no-op.
- `std::string device_name;` — the backend port name captured at open time, used
  to match a removed key to the open port(s). (Index is **not** used for
  matching — it is volatile; the match is on `(direction, device_name)`.)

### 4.3 Router — refresh control op

- New `ControlOpType::REFRESH`. `refresh_ports()` (public, thread-safe) enqueues
  a REFRESH op and waits ≤ 500 ms; the I/O thread, on REFRESH, runs the same
  re-enumeration + diff immediately and fulfills the op with the **count of
  changed ports** (added + removed). A timeout is surfaced via
  `notify_port_error(-1, ...)`.

### 4.4 Signal lifecycle (pinned)

- **Startup: annotated.** The first re-enumeration emits `midi_port_added` for
  every device present at init. (Chosen over a silent baseline.)
- **Changes while running:** only genuine set changes fire. A device removed
  then re-plugged fires `midi_port_added` again (fresh index); it is **not**
  auto-reopened.
- The first enumeration is the baseline; subsequent ticks diff against it.

### 4.5 Backend interface

**No change.** `MidiBackend` already exposes `list_ports()`; the router reuses it.
The `MidiBackend::PollResult::FATAL` path is unchanged (it remains for genuine
fatal stream errors; device-removal detection is the diff's job, per §2.2).

### 4.6 GDScript API (`libpd_server.{h,cpp}`)

New on `LibpdServer`:

- **Signals** (emitted from `_drain_midi_events()`, main thread):
  - `midi_port_added(kind: String, index: int, name: String)` — `kind` is
    `"input"` or `"output"`.
  - `midi_port_removed(kind: String, index: int, name: String)`.
- **Method** `midi_refresh_ports() -> int` — forces an immediate
  re-enumeration+diff on the I/O thread; returns the number of changed ports
  (added + removed; 0 = no change).
- **Property** `midi_port_poll_interval: float` (seconds; default `0.5`) —
  settable; stored in an atomic the I/O loop reads.
- `midi_list_inputs()` / `midi_list_outputs()` are **unchanged**, but their
  doc note is amended: *indices are stable only while the device set is
  unchanged — re-query after any `midi_port_added` / `midi_port_removed`.*

Plumbing mirrors `on_port_error`: a new router callback
`on_port_changed(added, kind, index, name)` (fires on the I/O thread) → server
lambda copies into a `pending_port_events_` ring under `midi_mutex` → emitted in
`_drain_midi_events()`. As with `on_port_error`, the callback must **never call
router methods from the I/O thread** (re-entrancy contract).

### 4.7 Error handling

- **Dead open port:** detected only by the diff (host backends don't `FATAL` on
  unplug). On a *confirmed* removal, the matching `real_device` port is closed →
  `midi_port_error(port_id, "device removed")` + `midi_port_removed(...)`.
  Transient empty `list_ports()` is debounced (§4.1), so a bad tick never
  closes live ports and a real "last device" unplug is still caught.
- **Refresh timeout:** reuses the 500 ms control-op wait; a timeout is surfaced
  via `midi_port_error(-1, ...)`.
- **Name collision:** two ports with the same `(direction, name)` are treated as
  one key. Rare; documented. (A future stable-token design would disambiguate.)

## 5. Threading invariants

- **All** backend port operations — including re-enumeration's `list_ports()`
  and any close-on-removal — run on the MIDI I/O thread.
- **Signals cross to the main thread** via the event ring; never emitted from
  the I/O thread (same as `midi_port_error`).
- Port records and `seen_`/`last_index_` are mutated **only** on the I/O thread
  (no lock needed after creation, consistent with existing `Port` handling).
- `pending_port_events_` is protected by `midi_mutex` (API-thread fill from the
  callback, main-thread drain).

## 6. Reconnect behavior

- A re-plugged device emits `midi_port_added` with a **fresh** index.
- We do **not** auto-reopen (rejected option C). The app re-opens + re-routes.
- If an open port had been auto-closed on removal, the app re-opens the new
  index when it handles the `midi_port_added`.

## 7. Testing

### 7.1 Fake-backend unit tests (extend `tests/midi_backend_fake_tests.cpp` + fake)

1. **Startup annotation** — a backend starting with N ports emits
   `port_added` for all N on the first tick.
2. **Add diff** — a port appears mid-run → exactly one `port_added`, correct
   kind/index/name.
3. **Remove diff + auto-close** — a `real_device` input disappears →
   `port_removed` fires **and** the open port is closed + `midi_port_error`;
   a virtual port with the same name is *not* closed.
4. **Index shift** — a removal re-indexes a surviving port; the survivor is
   *not* spuriously removed (proves name-keyed diff, not index-keyed).
5. **Refresh** — `refresh_ports()` returns the correct change count.
6. **Poll interval** — with a short interval the diff runs; events land.
7. **Transient-empty debounce** — `list_ports()` returns empty for exactly one
   tick then recovers → **no** `port_removed`, no auto-close (the blip is
   ignored). Returns empty for **two** consecutive ticks (the only device
   unplugged) → `port_removed` fires + the open `real_device` port is closed.
8. **Virtual/loopback not auto-closed** — a virtual (or loopback) port
   disappears from the list → `port_removed` fires but the (already-closed)
   non-`real_device` port is left untouched; a `real_device` port with the
   same name **is** closed.

### 7.2 Host smoke (device-free safe)

- Extend the MIDI smoke to a scripted hotplug scenario where a host MIDI server
  is present (e.g. macOS IAC bus add/remove); **SKIP cleanly** when no host MIDI
  server is present so device-free hosts stay green.
- Baseline audio smoke (`SMOKE_OK`) must remain green.

### 7.3 On-device

- **A133 (ALSA):** create/destroy an `aconnect`-visible virtual (or a
  synthetic) device → observe `midi_port_added`/`midi_port_removed` on stdout;
  the open-port auto-close path when the device disappears.
- **Android (RG DS):** loopback device add/remove → `midi_port_added` /
  `midi_port_removed` in logcat (GDScript `print()`; recall C-level `printf` is
  not routed to logcat on Android).

## 8. Files touched

- `extension/src/midi_router.{h,cpp}` — re-enum + diff, `Port.real_device` +
  `device_name`, `REFRESH` control op, `on_port_changed` callback, members
  (§4.1–4.3).
- `extension/src/libpd_server.{h,cpp}` — two signals, `midi_refresh_ports()`,
  `midi_port_poll_interval` property, `pending_port_events_` + emit in
  `_drain_midi_events()`, amended list-method docs (§4.6).
- `extension/src/midi_backend.h` — **doc-only** note on index stability
  (no interface change).
- `extension/tests/` — fake-backend hotplug tests (§7.1).
- `test_project/` — `test_midi` hotplug scenario (smoke) + wiring.
- `extension/README.md`, `docs/knulli-build.md`, `docs/android-build.md` —
  hotplug docs.

## 9. Verification targets

- macOS: ctest green (new fake-backend hotplug tests); `SMOKE_OK`; on a host
  with a MIDI server, the add/remove smoke fires both signals.
- A133 (ALSA): `midi_port_added`/`removed` observed; open-port auto-close
  confirmed.
- Android (RG DS): `midi_port_added`/`removed` in logcat; clean exit, zero
  crashes.
- PortMIDI fallback build (`MIDI_BACKEND=portmidi`): ctest green; host smoke
  fires the signals.

## 10. Open questions (resolved in design)

- **Startup: annotated** (emit `midi_port_added` for the initial set) — chosen.
- **No auto-reopen** — chosen.
- **Default poll interval 0.5 s** — balances Android's heavier per-call JNI cost
  against USB plug being a human-scale event; tunable via
  `midi_port_poll_interval`.
- **Name-keyed diff** (not index-keyed) — chosen, because indices are volatile.

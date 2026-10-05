# Receive hooks: patch → GDScript message signals (bang/float/symbol/list)

Status: **COMPLETE** (2026-10-05)
Spec: `docs/superpowers/specs/2026-10-05-godot-libpd-receive-hooks-design.md`

## Context / rulings

- libpd receive hooks fire ONLY for host-bound receivers (`libpd_bind`);
  patch-side `[receive]` objects never fire them (upstream behavior).
  Feature = **host-side subscription** over the official libpd API; no pd
  source patching. Patches use `[send name]` / bare `name;` lines.
- All four types: BANG, FLOAT (doubles fold in), SYMBOL, LIST (≤16 items,
  float-or-symbol per item, symbols ≤31 chars — documented limits).
- Delivery = server-level signals (`instance_id` first, like
  `instance_print`); no filtering at C level.
- Pipeline = existing print-hook path (worker hook → PdEvent → ring → main
  drain → signal); allocation-free hook path; drop-oldest back-pressure
  unchanged.
- Subscription is instance-level: `subscribe_receiver(name)` /
  `unsubscribe_receiver(name)`; binds run on the worker thread (thread-local
  `pd_this`); per-instance symbol table ⇒ no cross-instance name collision;
  duplicate subscribe fails; unsubscribe idempotent; all unbound on worker
  shutdown.
- Name stealing documented: a bound name must not also be `[receive]`d in
  the same instance's patch (dedicated `gd.*` send names).
- LIST Godot payload: mixed `Array` (float64/String — value type is the
  identifier).
- No device gate: host tests + test app.

## Progress log

- 2026-10-05: T1–T4 complete (message_hook_tests 15/15 host suite green, MSG_RECV_OK on macOS headless, Android cross-build green, probe scratch removed). User ruling: LIST in v1 with typed-tuple payload (Godot value type = identifier), 16-item cap documented.
- 2026-10-05: T1 red test written; first GREEN attempt failed (0 events).
  Root-caused with a standalone probe: `libpd_openfile` opens `dir/name`
  literally (no `.pd` extension — worker mkstemp patches are extensionless,
  which is why they load), and — decisively — **receive hooks only fire for
  `libpd_bind()` receivers** (spec rewritten to the subscription model;
  LIST moved into v1 per user ruling).

## Tasks

### T1 — `PdEvent` extension + worker hooks + SUBSCRIBE/UNSUBSCRIBE (TDD)
- [ ] `tests/message_hook_tests.cpp` rewritten for the subscription model
      (red first): subscribe → bang/float/symbol/list → events; patch
      `[r trig] → [send test.send]` path; truncation; unsubscribe;
      duplicate-subscribe failure.
- [ ] `pd_event_ring.h`: BANG/FLOAT/SYMBOL/LIST + fields + list arrays +
      `pd_event_truncate_utf8` helper.
- [ ] `libpd_worker.{h,cpp}`: c_bang/c_float/c_symbol/c_list trampolines +
      emit_*; install at both INIT sites (doublehook dropped — dead while
      floathook is set); SUBSCRIBE/UNSUBSCRIBE opcodes + bound map +
      teardown unbind.
- [ ] GREEN + full host suite green.

### T2 — Instance API + server signals + drain
- [ ] `LibpdInstance.subscribe_receiver/unsubscribe_receiver` (command
      push + promise wait).
- [ ] `libpd_server.{h,cpp}`: `instance_bang/float/symbol/list` signals +
      drain cases (LIST builds mixed `Array`).

### T3 — Test app + verification
- [ ] `test_messages.gd/.tscn` + `msgrecv.pd`; macOS headless run →
      `MSG_RECV_OK`; revert main scene.

### T4 — Docs + commit
- [ ] README section; spec status → COMPLETE; single commit.

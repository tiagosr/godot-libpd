# Receive hooks: patch → GDScript message signals (bang/float/symbol/list)

Status: **COMPLETE** (2026-10-05, implemented — commit see plan progress log)

## Problem

Patches can only talk to GDScript via `print` + `instance_print`. The vendored
libpd (v0.16.1) exposes receive-object hooks (`libpd_set_banghook/floathook/
symbolhook/listhook`) for exactly this purpose; only the print hook is wired.

## Recon finding (decides the design)

**libpd receive hooks fire ONLY for host-bound receivers** (`libpd_bind(name)`
creates a virtual `[r name]` that forwards to the hooks — `x_libpdreceive.c`,
confirmed in the vendored header docs and in pd 0.56-5's own `receive` class,
which never calls the hooks). Patch-side `[receive]` objects do NOT fire them
(upstream libpd behaves identically).

Consequence: the feature is **host-side subscription** over libpd's official
API. Patches send to names with `[send name]` (or bare `name;` message lines);
GDScript subscribes to those names on the instance. No pd source patching.

## Rulings

1. **Message types: bang, float, symbol, LIST (all four).** List payload is a
   typed tuple: at most **16 items**, each float or symbol (symbol text
   truncated to 31 chars). The 16-item cap and symbol length are documented
   limitations. Pointer items (if any appear) are dropped.
2. **Delivery: server-level signals** — `instance_bang`, `instance_float`,
   `instance_symbol`, `instance_list` — with `instance_id` first, matching
   `instance_print`'s shape. No receiver filtering; the subscribed name
   arrives in the payload. The list signal carries a mixed Godot `Array`
   (float64 / String elements — the Godot value type IS the type identifier).
3. **Pipeline = the existing print-hook path** — worker hook → `PdEvent` →
   `PdEventRing` → main-thread drain → signal. No new threads/queues/locks;
   the hook path stays allocation-free (LIST encodes into fixed arrays).
4. **Subscription API is instance-level** — `LibpdInstance.subscribe_receiver(
   name)` / `unsubscribe_receiver(name)`. Binds execute on the worker thread
   (thread-local `pd_this`; the instance's per-instance symbol table is
   correct there). Re-subscribing an already-bound name fails; unsubscribe is
   idempotent. All subscriptions are released on worker shutdown.
5. **Cross-instance name collision: none** — this build's symbol table is
   per-instance (`gensym` → `dogensym(..., pd_this)`), so the same name can be
   subscribed on multiple instances independently.
6. **Name stealing caveat (documented):** within one instance, a symbol binds
   to exactly one receiver. If the patch ALSO contains `[receive name]`,
   binding the same name makes the host's virtual receiver win the binding
   (whichever binds last). Host-bound names should be names the patch does not
   `[receive]` — e.g. dedicated `gd.*` send names. If a patch needs both
   local consumption and host capture, send to two names.
7. **Control-rate expectation:** hooks are control-rate (bangs, floats,
   symbols, short lists). Audio-rate processing stays in pd (mix-down).
8. **Double-precision messages arrive as FLOAT** (pd objects are `t_float`;
   the double hook would carry the same value — floathook is installed,
   doublehook left unwired as dead code).
9. **No device gate** — host tests + test app are the gate (pure
   worker/instance path, no platform code).

## Tasks

### T1 — `PdEvent` extension + worker hooks + subscribe commands (TDD)
- `tests/message_hook_tests.cpp` (NEW): worker-level test — INIT,
  `subscribe_receiver` (new SUBSCRIBE opcode), MESSAGE commands to bound
  names (bang / float / symbol / list), a patch with `[r trig] → [send
  test.send]` for the real `[send]` path, truncation, unsubscribe, duplicate
  subscribe. RED → GREEN.
- `pd_event_ring.h`: `BANG/FLOAT/SYMBOL/LIST` types + `sval[32]` + `fval` +
  list arrays (`n_items`, `list_floats[16]`, `list_syms[16][32]`,
  `list_is_symbol[16]`) + shared UTF-8-truncate helper.
- `libpd_worker.{h,cpp}`: `c_banghook/c_floathook/c_symbolhook/c_listhook`
  + `emit_bang/emit_float/emit_symbol/emit_list`; install at both INIT sites;
  SUBSCRIBE/UNSUBSCRIBE opcodes + bound-name map + teardown unbinds.

### T2 — Server signals + drain
- `libpd_server.{h,cpp}`: the four signals; drain-loop cases; LIST builds a
  mixed `Array` on the main thread.

### T3 — Test app + verification
- `test_project/data/msgrecv.pd`, `test_project/scenes/test_messages.tscn`,
  `test_project/scripts/test_messages.gd`: subscribe, `send_pd_message`,
  assert the four signals with correct payloads within 2 s;
  `MSG_RECV_OK` / `MSG_RECV_FAIL`; auto-quit. macOS headless run.

### T4 — Docs + commit
- `extension/README.md`: "Patch → GDScript messages" section (subscribe/
  unsubscribe, the four signals, send-side via `send_pd_message`, the
  stealing caveat, control-rate expectation, list limits).
- Spec status → COMPLETE; single commit.

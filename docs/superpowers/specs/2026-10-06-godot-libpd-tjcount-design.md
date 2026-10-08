# tjcount — in-house external: up-only ranged counter with wrap bang

Status: **COMPLETE** (2026-10-06, macOS verified — ctest 17/17 incl.
tjcount cases + Godot `--smoke`)

## Ruling (what this is)

`tjcount` is the first object of the **tj externals** series (ganged,
reloadable, ranged up-counters). It is deliberately a *self-contained
classic external*: the same `tjcount.c` builds (a) as a plain Pd /
Plugdata external via its own classic makefile, and (b) into the
embedded libpd build via `extension/cmake/pdexternals.cmake`.
Placed at `externals/tjcount/` in this repo; if the series grows, the
directory moves verbatim to its own repo/submodule.

## Semantics (agreed with the user)

| input | behavior |
|---|---|
| `bang` → inlet 0 | count + 1; if that would exceed `max`: **wrap** — count = min, **right-outlet bang FIRST**, then min on the left outlet (pd right-outlet-bang convention) |
| `float` → inlet 0 | count = **clamped** float (into [min, max]), output on left |
| `set f` → inlet 0 | count = clamped f, **no output** |
| `min f` / inlet 1 | new min, accepted only while `min < max` (else error posted, unchanged); count clamped into new range, no output |
| `max f` / inlet 2 | symmetric |
| creation `[tjcount min max]` | optional floats, default `0 1`; invalid range → note + `0..1`; no loadbang, quiet start |

Increment fixed +1 (up-only; no down/up-down/alt — unlike cyclone's
`counter` and else's `count`, whose carry fires when a boundary is
*reached* — one count too early for the user's ganged-chain use; the
wrap bang fires exactly on `max -> min`).

Outlets: left = float count; right = wrap bang. Inlets: 0 = left,
1 = min, 2 = max.

## Files

- `externals/tjcount/tjcount.c` — the object (vanilla-API:
  `class_new`/`class_addbang`/`class_addfloat`/`class_addmethod`,
  `inlet_new`/`outlet_new`; compiles with and without
  `PDINSTANCE/PDTHREADS/PD_INTERNAL`).
- `externals/tjcount/makefile` — classic external makefile
  (`include $(PD)/src/rules`), works against vanilla Pd 0.4x–0.6x and
  Plugdata.
- `externals/tjcount/tjcount.pd`, `README.md` — doc stub + usage/build
  instructions.
- `extension/cmake/pdexternals.cmake` — new section: static lib
  `tjcount` (same `PDINSTANCE` defines as cyclone/pdelse).
- `extension/src/core/pdexternals.cpp` — `register_pdexternals()` now
  also calls `tjcount_setup()`.
- `extension/CMakeLists.txt` — `tjcount` linked into the dylib and all
  host tests that link the externals.
- `extension/tests/external_tests.cpp` — `test_tjcount()`: values
  `1,2,3,wrap 0,1,clamp 3,set 1→bang 2,rejected min→bang 3`, exactly
  one wrap bang, and the wrap bang event arrives **before** the wrapped
  count event.

## Bug found en route (worker message dispatch)

The test exposed a real bug in `LibpdWorker`'s `MESSAGE` command
delivery (not in tjcount): multi-atom **symbol-first** messages (e.g.
`set 1`, `min 5`) were sent via `libpd_list()`, which delivers a pure
`s_list` — and a pure list does **not** trigger class-method dispatch
(only `pd_typedmess` does; that's how a patch message box delivers).
Such messages were silently dropped at any destination.

Fix (`src/libpd_worker.cpp`): the MESSAGE path now sends
symbol-first multi-atom messages via `libpd_message()` (selector
dispatch, message-box semantics) and keeps `libpd_list()` only for
float-first messages (pure lists; the host LIST receive hook still
fires for them — `message_hook_tests` regression-checked).

## Verification

- ctest 17/17 (macOS), incl. the tjcount cases above.
- Portability: `clang -std=c99 -Wall -O2` compile of `tjcount.c`
  against the vendored vanilla-API pd headers **without**
  `PDINSTANCE` (the plain-Pd/Plugdata mode) is clean. A full classic
  `.so` build was not run here — the vendored libpd tree ships no
  `src/rules`/shared pd lib; the makefile is the standard template.
- Godot `--smoke` (macOS): SMOKE_OK after the worker dispatch fix.

## Open items

- The classic makefile should get a real on-Plugdata/vanilla-Pd build
  pass on a machine with a full pd source tree (cheap follow-up).
- Series roadmap (later): a ganged bank object (N staged tjcounts in
  one object, advanced by one bang) reusing this counter core.

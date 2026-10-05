# Abstraction arguments: `$1 $2 …` instantiation of synth patches

Status: **ACTIVE** (2026-10-05)

## Problem

Multi-track design: one `LibpdInstance` per track, each loading the same
synth abstraction with a per-instance index so the patch can name its
internal receivers/senders by it — `[r track-$1.volume]` becomes
`track-3.volume` for instance 3 (which GDScript can then subscribe to,
receive-hook feature). Normal pd semantics: `[synth 1 2 3]` binds
`$1=$2=$3` in the instantiated abstraction.

## Rulings

1. **API: `LibpdInstance.load_abstraction(name, args) -> Error`** —
   coexists with `load_patch(path)` (plain top-level load, unchanged).
   `name` may be a bare name or a path; `args` are simple tokens
   (same space-joined convention as `send_pd_message`; no `;`/`,`/whitespace
   inside a token).
2. **Mechanism = generated loader canvas, no libpd/pd API change.**
   `libpd_openfile` cannot pass creation arguments, so the worker (owning
   the pd instance and thread-local `pd_this`) writes a temp top-level
   patch containing `#X obj 10 10 <name> <args…>;` to /tmp, adds the
   abstraction's directory to the search path, loads it, and deletes the
   temp file (pd keeps the parsed binbuf in memory). pd's standard
   abstraction machinery does the rest: unknown class → `sys_load_lib`
   opens `name.pd` from the search path; `binbuf_eval` substitutes `$n`
   from the creation arguments (verified present in the vendored
   pd 0.56-5: `new_anything` → `sys_load_lib` → `canvas_open(..., ".pd")`;
   `A_DOLLAR`/`A_DOLLSYM` handling in `binbuf_eval`).
3. **The abstraction file keeps its normal `name.pd` spelling** (pd
   auto-loads with the `.pd` extension); the loader references the bare
   name (extension stripped).
4. **One instance = one top-level instantiation.** Multiple instances of
   the same abstraction in one worker are not supported (same as
   today's single top-level patch); the class is cached in the instance's
   per-instance symbol table after first load — separate
   `LibpdInstance`s are independent.
5. **Close semantics unchanged** — `close_patch`/UNLOAD frees the loader
   canvas; the instantiated abstraction's objects are freed with it
   (pd canvas-free cascades).
6. **No server changes** — messaging the instantiated receivers uses the
   existing `send_pd_message` + `subscribe_receiver` (receive hooks).
7. **Gate: host worker test + test app** (no platform code).

## Tasks

### T1 — worker-level TDD test (red → green)
`extension/tests/abstraction_tests.cpp`: temp dir + `synth.pd`
(`[r synth-$1] → [s host.out]`); LOAD with args → subscribe `host.out`
→ send to `synth-42` → BANG event (proves substitution — a failed
substitution would leave `synth-42` unreceived). Multi-arg
(`m-$1-$2` → `m-7-8`). Regression: plain no-args LOAD still loads.

### T2 — worker LOAD-with-args (loader generation)
`libpd_worker.cpp` LOAD case: non-empty `args` → strip ext, temp loader
canvas in /tmp (mkstemp), add dir to search path, openfile, delete temp.

### T3 — instance API
`LibpdInstance.load_abstraction(name, PackedStringArray args) -> int`
(pushes LOAD with space-joined args) + method binding.

### T4 — test app + docs + commit
`test_abstraction.gd/.tscn` + `data/abstr.pd` → `ABSTRACTION_OK` on
macOS headless; README section; full host suite + Android cross-build.

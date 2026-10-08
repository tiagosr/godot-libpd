# tjlistfind — in-house external: 1-indexed list find

Status: **COMPLETE** (2026-10-08, macOS verified — ctest 17/17 + Godot `--smoke` rc=0)

Second object of the **tj externals** series (see
`2026-10-06-godot-libpd-tjcount-design.md`). Same dual-build shape as
`tjcount`: one `tjlistfind.c` that builds as a classic Pd/Plugdata
external (own makefile) and as a static lib into the embedded build
(`extension/cmake/pdexternals.cmake`, registered in
`register_pdexternals()`).

## Semantics (per user request)

`tjlistfind <target> <tolerance>` — takes a **list of floats** as a
message and outputs the **1-indexed** position of the first float
matching the argument; `0` when absent.

- list input: atoms scanned left to right; non-float atoms are skipped;
  first `|v - target| <= tolerance` wins → output `i+1` (float)
- single float input: one-element list (`1` if match, else `0`)
- `find f`: change the target (no output)
- `tolerance f`: absolute tolerance, `f >= 0` (no output); creation
  args: target required-in-practice (default 0), tolerance default 0
  (exact match)
- single float outlet

## Files

- `externals/tjlistfind/tjlistfind.c` — the object
- `externals/tjlistfind/makefile`, `tjlistfind.pd`, `README.md`
- `extension/cmake/pdexternals.cmake` — static lib `tjlistfind`
- `extension/src/core/pdexternals.cpp` — `tjlistfind_setup()`
- `extension/CMakeLists.txt` — linked into dylib + all test targets
- `extension/tests/external_tests.cpp` — `test_tjlistfind()`:
  positions `2,3,1,0,0,2,1` (exact object) and `0,0,0,0,2,2,0`
  (tolerance object) across list / single-float / `find` / `tolerance`
  inputs, symbol skipping, no-match → 0.

## Pd-API gotchas hit (for the series' reference)

1. **Struct layout**: first member must be an *embedded* `t_object x_obj;`
   (tjcount's pattern). A leading `t_object *` pointer overlaps the
   header `pd_new` writes → `pd_checkobject` fails →
   "didn't return a patchable object".
2. **New function must RETURN the object**: this libpd fork's
   `mess_dispatch` reads the new function's return value into
   `pd_this->pd_newest` (vanilla pd externals return `void *` and rely
   on the same convention). A `void` new function leaves `pd_newest`
   unset/garbage.
3. **List method signature** in this pd is
   `void fn(t_pd *x, t_symbol *s, int argc, t_atom *argv)` (4 args,
   selector included) — registered via the `class_addlist` macro that
   casts to `t_method`.
4. `atom_gettype()` and `error()` are *not* in this fork's public
   `m_pd.h`; use the `a_type` field and `post()` (both portable).
5. `class_addlist`'s `(t_listmethod)` cast type doesn't exist in this
   header; the macro does the cast.

Verification: ctest 17/17; clean `clang -std=c99 -Wall` in plain-Pd
mode (no PDINSTANCE); smoke rc=0.

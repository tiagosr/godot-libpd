# Vendored pd externals: cyclone + else in the libpd build

Status: **COMPLETE** (2026-10-06, macOS verified — ctest 17/17 + Godot `--smoke`)

## Problem

Patches loaded through libpd could only use vanilla pd 0.56-5 objects. The
user wants the porres externals (cyclone + else) — control- and audio-rate
objects — available in every libpd-loaded patch, with any GUI object whose
graphical part cannot be stubbed out / NOP'ed **blacklisted from the build**.

## Submodules

- `extension/thirdparty/cyclone` — https://github.com/porres/pd-cyclone.git (master)
- `extension/thirdparty/else` — https://github.com/porres/pd-else.git (master)

Both build as static libraries (`cyclone`, `pdelse`) in
`extension/cmake/pdexternals.cmake`, compiled against the vendored (stripped)
pd headers, and are linked into the `godot_libpd` dylib and every host test
that links `src/libpd_worker.cpp`.

## Registration (one call per process)

`src/core/pdexternals.cpp` → `register_pdexternals()` calls `cyclone_setup()`
(its generated `single_lib.c` registers every cyclone object; `cyclone_setup`
itself registers the non-alphanumeric `!-`, `!/`, … objects) and
`register_else_objects()` (generated `else_register.c` calls every
`*_setup()` found in the built else sources). Called from
`LibpdWorker::init_pd_globals_once()` after `libpd_init()`. `class_new()`
registers into the global class list **and** into the method tables of every
existing instance (`class_doaddmethod` iterates `pd_ninstances`); instances
created later inherit via `pdinstance_new()` — so a single process-level call
covers all instances.

## Cyclone build

All `cyclone_objects/binaries/audio/*.c` + `control/*.c` that contain
`CYCLONE_OBJ_API` (this filter drops `audio/scope_dialog.c` — a Tcl `sys_gui()`
script dump, not a compilable object). Plus the upstream `cyclone_shared`
support files (file/grow/magicbit/random, gui/mifi/rand/tree, cybuf).

The generated `single_lib.c` (upstream `single_lib.c.in` template) declares and
calls the **full `*_setup` function names** — never the bare object names,
which can be C keywords (`switch`). `PDINSTANCE=1 PDTHREADS=1 PD_INTERNAL=1`
must match the libpd core, or externals reference global `s_bang`/`s_`/…
that do not exist in `libpd-multi.a` (there they are `pd_this->pd_s_*`
members).

## Else build + blacklists

Top-level `Source/{Audio,Control,Shared}/*.c` plus `Shared/fftease/*.c`
(pvretune~'s self-contained phase-voice resynthesis lib).

**Blacklisted (cannot render / useless headless or dep-heavy):**

| file | reason |
|---|---|
| `Control/else.c` | bootstrap: `lua_setup()` + TCL GUI plugin loading |
| `Audio/pdlink~.c`, `Control/pdlink.c` | link-protocol objects pulling the C++/UDP `Source/Shared/link` lib |
| `Audio/beat~.c` | aubio-dependent |
| `Audio/streamin~.c`, `streamout~.c`, `play.file~.c` | ffmpeg-dependent |
| `Control/sfinfo.c`, `sfload.c` | soundfile (ffmpeg/vorbis) backend |
| `Control/popmenu.c`, `Audio/numbox~.c` | GUI-only: their entire purpose is a dialog that can never appear in the embedded build |

**GUI objects KEPT (deliberate ruling):** `knob`, `button`, `pad`, `bicoeff`
— canvas-drawn widgets whose drawing goes through `pdgui_vmess()`, a no-op in
this headless pd build. `knob` in particular is also a full message-driven
control object (`set`/`inc`/`dec`/`range`/`jump`/`param`/`discrete`); its
double-click dialog is `pdgui_stub_vnew` bookkeeping that never renders but
cannot crash. The user required keeping knob in this no-op mode.

Vanilla pd's own GUI objects (`hslider`, `table`, …) need no blacklisting —
this libpd fork already ships a stripped `x_gui.c` without them. Cyclone has
no GUI objects.

**Symbol-collision rulings** (static-archive duplicates → link errors):

| collision | resolution |
|---|---|
| else `tanh`/`trunc`/`atan2~`/`car2pol~`(+`cartopol` alias)/`pol2car~`/`scope~` vs same-named cyclone objects (same class names **and** C symbols) | else copies excluded; cyclone kept |
| else `messcoll.c` (`coll_checkint` is global in cyclone's `coll.c` too) | else `messcoll` excluded (GUI coll widget; cyclone `coll` covers it) |
| else `Shared/elsefile.c` vs cyclone `shared/common/file.c` (identical krzYszcz file API) | generated `elsefile_nodup.c` wrapper renames else's raw `osdir_*`/`ospath_*`/`fileread_open`/`filewrite_open`/`panel_*`/`embed_save` to `else_*`-prefixed copies; else objects keep the `elsefile_*` API and bind raw names to cyclone's identical impl |
| else `Shared/s_elseutf8.c` vs pd core `s_utf8.c` (plain `u8_*`) | generated `s_elseutf8_nodup.c` wrapper renames the plain `u8_*` set to `else_u8_*`; the `else_u8_*` API (else objects) is untouched |
| cyclone `shared/control/s_cycloneutf8.c` vs pd core `s_utf8.c` | excluded; generated `comment_nodup.c` wrapper compiles cyclone's `comment.c` with `cyclone_u8_*` mapped back to core `u8_*` |
| else `Shared/mifi.c` vs cyclone `shared/control/mifi.c` | else copy excluded; else `midi.c` binds cyclone's mifi API |

## Godot-side smoke extension

`test_project/data/external_patch.pd` (else `op + 1` on `notein` → `print`,
cyclone `phasor~` → `clip~` → `dac~`) is loaded in the `--smoke` flow
(step 2b) and the `60+1=61` print is asserted (step 4).

Two pre-existing smoke gaps on NATIVE_AUDIO builds were fixed to make the
smoke meaningful on the host:

1. `debug_blocks_pushed` reported the Godot sink (always 0 when the worker
   renders into `synth_ring`); it now reports the ring's block count
   (`MixInputRing::blocks_pushed()`) on NATIVE_AUDIO builds.
2. `--smoke` never opened the PortAudio stream nor bound a mixer; the
   kick-driven SYNTH worker renders nothing without both. Smoke step 2c now
   mirrors `test_native_mix`: `audio_open(256, rate)` + MIXER instance on
   `mixdown_16.pd` + `set_mixer`, freed + `audio_close()` at the end.

## Verification

- `./build.sh --macos` clean; `ctest` 17/17 (incl. new `external_tests`:
  control chain across both libs, non-alphanumeric `!-`, audio-rate
  `phasor~` → DrySink peak > 0).
- Godot 4.6 `--smoke` on macOS: SMOKE_OK — externals confirmed in-process
  (else `op`: 60→61; cyclone `phasor~` rendered 236+ blocks through the
  PortAudio mix path).
- Linux/Android: build wiring is platform-neutral (static libs, same
  defines); not rebuilt on this machine.

## Open items / risks

- cyclone prints a version warning ("Cyclone 0.9-6 needs at least Pd 0.57-0,
  you have 0.56-5") and dumps Tcl text (`sys_gui` no-op path) at init —
  cosmetic console noise, no functional impact.
- Submodules track master; a future pin/upgrade may re-introduce new
  duplicate-object or dependency cases — the filter/blacklist lists in
  `pdexternals.cmake` are the single place to adjust.

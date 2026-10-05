# Abstraction arguments: `$1 $2 …` instantiation

Status: **COMPLETE** (2026-10-05, implemented)
Spec: `docs/superpowers/specs/2026-10-05-godot-libpd-abstraction-args-design.md`

## Progress log

- 2026-10-05: Recon verified the vendored pd 0.56-5 keeps pd's standard
  abstraction machinery (`new_anything` → `sys_load_lib` → `canvas_open
  (".pd")`; `A_DOLLAR`/`A_DOLLSYM` substitution in `binbuf_eval`). T1
  worker test red (loads succeed, no substitution) → T2 loader
  generation green (single-arg `synth-42`, multi-arg `m-7-8`, no-args
  regression) → T3 `LibpdInstance.load_abstraction(name, args)` →
  T4 test app `ABSTRACTION_OK` on macOS headless, 16/16 host suite,
  Android arm64-v8a cross-build green.

# godot-libpd GDExtension

Multi-instance, worker-threaded libpd (Pure Data) GDExtension for Godot 4.6,
with Godot-native audio (AudioStreamGenerator). See
`../docs/superpowers/specs/2026-09-27-godot-libpd-gdextension-design.md`.

## Pinned third-party versions

| Submodule | Pin |
|---|---|
| `thirdparty/godot-cpp` | branch `4.5`, commit `27d9dd2` (godot-4.5-stable-30-g27d9dd2) |
| `thirdparty/libpd` | `ba0dc63` (libpd 0.16.1, pd vanilla 0.56-5; pure-data submodule @ f009fd8) |
| `thirdparty/portmidi` | `6b51c25` (compiled in, wiring is v2) |

## Building

```
./build.sh --macos          # extension/build/macos/libgodot_libpd.dylib
./build.sh --linux-arm64    # extension/build/linux/libgodot_libpd.so  (inside Knulli Docker)
./build.sh --android        # extension/build/android-arm64/libgodot_libpd.so (NDK in $NDK)
```

Note: CMake >= 4.x may require `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
depending on godot-cpp's scripts (tracked here if needed).

## Testing

Headless tests live in `../test_project` (main scene switched per test;
see the implementation plan). Unit tests: `ctest --test-dir build/cmake-macos`.

## Multi-instance MIDI hook attribution

Each `LibpdInstance` owns a `LibpdWorker` thread, and that thread is the
ONLY thread that ever calls into libpd for that instance: the worker
creates its own pd instance during INIT (`libpd_init` ->
`libpd_new_instance` -> `libpd_set_instance`) and executes every command
(MIDI input, load/unload, dsp) on that thread.

Output hooks fire synchronously on the thread that triggered the output
(`libpd_noteon` etc. run to completion on the caller). The hook
trampolines in `src/libpd_worker.cpp` resolve their owning worker via
`libpd_get_instancedata()`, which under a multi-instance build resolves
per instance: libpd's `LIBPDSTUFF` is `STUFF->st_impdata` (per instance)
whenever `PDINSTANCE` is defined, and `pd_this` — the current instance —
is thread-local (`__thread` in `pure-data/src/m_pd.h`). So every hook
event lands in the emitting instance's own worker `midi_out` queue, and
routing fan-out (A -> MIDI port -> B) delivers each note to exactly the
instances routed from that port.

This REQUIRES the multi-instance build: `PD_MULTI` is forced ON in
`extension/CMakeLists.txt` (see the guardrail comment there). With
`PD_MULTI OFF`, `LIBPDSTUFF` falls back to the single process-wide
`libpd_mainimp`, and every instance's hooks/instancedata collapse into
one — notes from any instance would be attributed to the worker that
last called `libpd_set_instancedata`. Do not turn `PD_MULTI` off.

> Review note (2026-09-30 whole-branch review, P1): the review's concern
> that a single process-wide `i_data` misroutes multi-instance hooks was
> investigated and disproven for this build — `PD_MULTI ON` is forced,
> so the per-instance path above is the one that compiles. The
> regression tests in `tests/multi_instance_midi_tests.cpp`
> (`multi_instance_midi_tests` ctest target) lock in correct attribution:
> two real workers each drive their own instance, a note through one
> appears only in that worker's queue, and a routed A -> port -> B
> fan-out reaches only B.

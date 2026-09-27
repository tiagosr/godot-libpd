# Godot libpd GDExtension — Implementation Plan (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A multi-instance, worker-thread libpd GDExtension for Godot 4.6 with Godot-native audio, plus a custom linux-arm64 engine build for Knulli handhelds and a test app proving it on macOS, Android, and the Trimui Brick (A133).

**Architecture:** godot-cpp (4.5, CMake) GDExtension with libpd (0.16.1, `PD_MULTI=ON`) and PortMIDI compiled in statically. Each `LibpdInstance` Node owns one pd instance, a per-instance command queue, and a worker thread that drains the queue and runs `libpd_process_float` at block cadence, pushing output into an `AudioStreamGenerator` (`play_from_thread`). A singleton `LibpdServer` Node owns the cross-thread event ring and emits all Godot signals on the main thread.

**Tech Stack:** C++17, godot-cpp 4.5 (CMake), libpd 0.16.1 / pd 0.56-5 (CMake, PD_MULTI=ON), PortMIDI (CMake), Godot 4.6.2 editor (user-installed) for exports, SCons for the engine, Docker `linux/arm64 ubuntu:22.04` (Knulli), NDK r26 (Android).

**Spec:** `docs/superpowers/specs/2026-09-27-godot-libpd-gdextension-design.md`

## Global Constraints

- Godot engine pin: tag `4.6-stable`. User's editor is 4.6.2; official 4.6.2 templates are used for macOS/Android — **do not build engine templates for those**.
- libpd pinned to submodule `modules/libpd` @ `ba0dc63` (v0.16.1). PortMIDI @ `6b51c25`. godot-cpp pinned to branch/tag `4.5` (commit recorded at checkout).
- All libpd/pd calls for an instance happen **only on that instance's worker thread**. Worker threads never call Godot APIs or emit signals — only push to the `LibpdServer` event ring.
- Audio v1 = `AudioStreamGenerator` + `play_from_thread()`, output only (`n_ins=0`). No native audio backends, no MIDI I/O, no audio input in v1.
- Test project `AudioServer` mix rate is 44100 (`audio/mixrate/mix_rate=44100` in `project.godot`).
- libpd DSP entry is `libpd_process_float(ticks, in, out)`; blocksize comes from `libpd_blocksize()` (pd-build-time fixed). Multi-instance requires `-DPD_MULTI=ON` on the libpd CMake build.
- Extension output locations: `extension/build/macos/libgodot_libpd.dylib`, `extension/build/linux/libgodot_libpd.so`, `extension/build/android-arm64/libgodot_libpd.so`.
- Docker containers run natively as `linux/arm64` on the Apple Silicon host (`docker build --platform linux/arm64`).
- Every task ends with a git commit. `dist/`, `extension/build/`, engine `bin/` are gitignored.

## Review Focus

1. **Samplerate mismatch** (instance srate ≠ `AudioServer.get_mix_rate()`) — expect `init()` to fail fast with a clear error, never garbage audio. (Tested in Task 5.)
2. **Instance freed while dsp is running** — expect clean stop+join in `_exit_tree`, no hang, no use-after-free. (Tested in Task 6.)
3. **Repeated spawn/kill of instances** — expect no memory growth or leaked pd instances over many cycles. (Tested in Task 6.)
4. **Main-thread calls while the worker is running** (`send_midi`/`send_pd_message` mid-dsp) — expect commands to be executed between dsp blocks on the worker thread, never dropped or raced. (Tested in Task 4 + 6.)
5. **Old glibc on Knulli A133/H700** (kernel 4.9) — expect the engine binary to actually start on-device; fallback ladder: static libc, then lowered glibc floor. (Device test in Task 9.)

---

### Task 1: Extension scaffold, CMake build, empty registration (macOS)

**Files:**
- Create: `extension/CMakeLists.txt`, `extension/src/register_types.{h,cpp}`, `extension/godot-libpd.gdextension`, `extension/build.sh`
- Create: `test_project/project.godot`, `test_project/addons/godot-libpd/godot-libpd.gdextension`, `test_project/scenes/boot_check.tscn`, `test_project/scripts/boot_check.gd`, `test_project/.godot/extension_list.cfg`, `test_project/.gitignore`
- Submodules: `extension/thirdparty/godot-cpp` @ `4.5`, `extension/thirdparty/libpd` @ `ba0dc63`, `extension/thirdparty/portmidi` @ `6b51c25`
- Test: `test_project` headless boot check

**Interfaces:**
- Produces: a buildable shared library `libgodot_libpd.dylib` registering an empty `LibpdServer` class (a `Node` subclass with no members yet); `build.sh --macos` entrypoint.
- Registers types via the godot-cpp `GDREGISTER` pattern in `register_types.cpp`.

- [ ] **Step 1: Add the three pinned submodules**

```bash
cd extension && git submodule add -b 4.5 https://github.com/godotengine/godot-cpp.git thirdparty/godot-cpp
git submodule add https://github.com/libpd/libpd.git thirdparty/libpd
cd thirdparty/libpd && git checkout ba0dc63 && git submodule update --init   # fetches pure-data
cd ../.. && git submodule add https://github.com/PortMidi/PortMidi.git thirdparty/portmidi
cd thirdparty/portmidi && git checkout 6b51c25
```

Record the godot-cpp commit hash in `extension/README.md` (one line: pinned commit).

- [ ] **Step 2: Write `extension/CMakeLists.txt`**

One shared library target `godot_libpd`:

- `add_subdirectory(thirdparty/godot-cpp EXCLUDE_FROM_ALL)` → link the static target `godot-cpp`.
- libpd: `add_subdirectory(thirdparty/libpd EXCLUDE_FROM_ALL)` with `-DPD_MULTI=ON -DPD_UTILS=OFF -DPD_EXTRA=OFF -DLIBPD_SHARED=OFF` — link the `libpd` static lib; also link `portmidi` (see Step 4 note).
- PortMIDI: `add_subdirectory(thirdparty/portmidi EXCLUDE_FROM_ALL)`, static, `PM_EXTRA_TESTS=OFF`; compile only what has a backend on the target platform (macOS: CoreMIDI; Linux: ALSA; Android: compile **nothing** — PortMIDI has no Android backend; gate via a `BUILD_PORTMIDI` option defaulting ON, set OFF for android).
- Output: `LIBRARY_OUTPUT_DIRECTORY = ${CMAKE_SOURCE_DIR}/build/<platform>` where `<platform>` ∈ `macos` (APPLE), `linux`, `android-arm64` (ANDROID_ABI arm64-v8a). Target name → `libgodot_libpd.{dylib,so}`.
- Sources for now: `src/register_types.cpp`.
- Register a minimal `LibpdServer` class (Node subclass, no API yet) so the .gdextension has something to register.

- [ ] **Step 3: Write `extension/build.sh`**

Dispatch on arg; v1 implements `--macos` (and `--linux-arm64`, `--android` as stubs that error "not implemented yet — Task 8/10"):

```
--macos: cmake -B build/cmake-macos -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake-macos
```

- [ ] **Step 4: Write the `.gdextension` files**

`extension/godot-libpd.gdextension` (canonical, for reference) and the copy used by the test project at `test_project/addons/godot-libpd/godot-libpd.gdextension` — same content, library paths relative to **each file's own location**:

```ini
[configuration]
version = 3
entry_symbol = "godot_libpd_library_init"
compatibility_minimum = 4.5
singleton = false
reloadable = true

[libraries]
macos.debug.release = "../../build/macos/libgodot_libpd.dylib"
linux.debug.release = "../../build/linux/libgodot_libpd.so"
android.arm64 = "../../build/android-arm64/libgodot_libpd.so"
```

(Adjust the macos/linux entry to the godot-cpp 4.5 `.gdextension` template's exact `libraries` keys if the format above doesn't load; verify in Step 6.)

- [ ] **Step 5: Create the test project skeleton**

`test_project/project.godot` for Godot 4.6 with:

- `config/name="godot-libpd-test"`, `config/features=PackedStringArray("4.6")`
- `run/main_scene="res://scenes/boot_check.tscn"`
- `audio/mixrate/mix_rate=44100`
- autoload `LibpdServer` pointing at `res://scripts/libpd_server.gd` (create that file now as `extends LibpdServer` with an empty class body)

`test_project/scenes/boot_check.tscn`: a `Node2D` root with script `boot_check.gd`.
`test_project/scripts/boot_check.gd`:

```gdscript
extends Node2D

func _ready():
    if not ClassDB.class_exists("LibpdServer"):
        push_error("LibpdServer class missing")
        get_tree().quit(1)
        return
    if LibpdServer == null:  # autoload instance
        push_error("LibpdServer autoload missing")
        get_tree().quit(1)
        return
    print("LIBPD_BOOT_OK")
    get_tree().quit(0)
```

`test_project/.godot/extension_list.cfg` (one line, so headless runs load the extension without an editor scan):

```
res://addons/godot-libpd/godot-libpd.gdextension
```

`test_project/.gitignore`: ignore `.godot/` **except** `.godot/extension_list.cfg` (write: `.godot/*` then `!.godot/extension_list.cfg`).

- [ ] **Step 6: Build and verify the extension loads**

Run: `cd extension && ./build.sh --macos`
Expected: `extension/build/macos/libgodot_libpd.dylib` exists.

Run: `godot --headless --path ../test_project --quit` (from `extension/`; `godot` = the user's 4.6.2 editor binary — adjust the command name if it isn't on PATH)
Expected: exit code 0, stdout contains `LIBPD_BOOT_OK`, no `Error loading extension` lines.

- [ ] **Step 7: Commit**

```bash
git add extension/ test_project/
git commit -m "feat(extension): scaffold godot-cpp CMake extension + boot test"
```

---

### Task 2: Event ring + command queue (pure C++, unit-tested)

**Files:**
- Create: `extension/src/core/pd_event_ring.{h,cpp}`, `extension/src/core/pd_command_queue.{h,cpp}`
- Test: `extension/tests/core_tests.cpp` (plain C++ `main()`, assertions, exit code — no godot-cpp, no external test framework)

**Interfaces:**
- Produces:
  - `PdEvent { int64_t instance_id; uint32_t type; char data[64]; }` — `type` ∈ {PRINT, NOTE_ON, DSP_ACTIVE}; PRINT carries UTF-8 text (truncated to 63 bytes), NOTE_ON carries `{ch, pitch, vel}` packed, DSP_ACTIVE carries bool.
  - `class PdEventRing { void push(const PdEvent&); size_t pop(PdEvent* out, size_t max); size_t count_approx() const; }` — bounded (1024), multi-producer single-consumer, drops oldest on overflow (never blocks the worker).
  - `class PdCommand { uint32_t opcode; /* payload: cstring path, args, floats */ }; class PdCommandQueue { void push(PdCommand); std::optional<PdCommand> pop(); void drain(std::function<void(PdCommand)>); bool empty() const; }` — thread-safe, multi-producer (only main thread in practice), single-consumer (worker), FIFO.
- Consumes: nothing (no Godot).

- [ ] **Step 1: Write the failing tests** (`extension/tests/core_tests.cpp`)

Tests (each a named function, all called from `main`, any failure → `push_error` + exit(1)):

1. `ring_fifo_single_thread`: push 100 events in order, pop all, assert exact order + count.
2. `ring_concurrent_no_loss`: 2 producers × 2000 events, 1 consumer until empty; assert total popped == 4000 and `count_approx()==0`.
3. `ring_overflow_drops_oldest`: capacity known (1024); push 2048, pop all, assert 1024 popped and the first popped is event #1024 (0-indexed 1023).
4. `queue_fifo`: push 100 commands from "main", drain; assert order.
5. `queue_concurrent_push`: main thread pushes 500 commands while a consumer thread pops; join, assert all 500 received exactly once.
6. `queue_empty_after_drain`: `empty()` true after full drain.

- [ ] **Step 2: Wire the test target into CMake**

In `extension/CMakeLists.txt`: `add_executable(libpd_core_tests tests/core_tests.cpp src/core/pd_event_ring.cpp src/core/pd_command_queue.cpp)` (guarded by `BUILD_TESTING`, default ON for host, OFF for cross builds) + `add_test(NAME core_tests COMMAND libpd_core_tests)`.

- [ ] **Step 3: Run tests to verify they fail**

Run: `./build.sh --macos && ctest --test-dir build/cmake-macos --output-on-failure`
Expected: FAIL (classes don't exist yet → compile error or link failure is acceptable here since tests were written first).

- [ ] **Step 4: Implement `PdEventRing` and `PdCommandQueue`**

Ring: fixed array + atomic head/tail (single-consumer pop; producers take a spinlock or atomic-cas slot reservation — pick the simpler correct one, document choice in a comment). Queue: `std::deque` under `std::mutex` + `std::condition_variable` (workers pop with a short timeout so stop checks stay responsive — e.g. `pop_timeout(2ms)`).

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --test-dir build/cmake-macos --output-on-failure`
Expected: all 6 pass.

- [ ] **Step 6: Commit**

```bash
git add extension/
git commit -m "feat(extension): thread-safe event ring + command queue with unit tests"
```

---

### Task 3: `LibpdServer` node (ring owner, main-thread signal fan-out)

**Files:**
- Create: `extension/src/libpd_server.{h,cpp}`
- Modify: `extension/src/register_types.cpp` (register `LibpdServer`), `extension/CMakeLists.txt` (add sources)
- Test: `test_project/scripts/test_server.gd` + `test_project/scenes/test_server.tscn`

**Interfaces:**
- Produces (Godot-visible, on `LibpdServer`):
  - Signals: `instance_print(instance_id: int, text: String)`, `instance_note_on(instance_id: int, channel: int, pitch: int, velocity: int)`, `instance_dsp_active(instance_id: int, active: bool)`
  - Methods: `instance_registered(instance_id: int) -> bool`, `instance_count() -> int`, `debug_push_print(instance_id: int, text: String)` (test hook that pushes directly into the ring — later tasks use the real hooks; this one is kept for signal testing)
  - Static C++: `LibpdServer::get_singleton() -> LibpdServer*` (the most-recently-created instance; the project uses exactly one, as an autoload).
- Consumes: `PdEventRing`, `PdEvent` from Task 2.

- [ ] **Step 1: Write the failing headless test**

`test_project/scripts/test_server.gd`:

```gdscript
extends Node2D

var got := {}

func _ready():
    LibpdServer.instance_print.connect(func(id, text): got["print"] = [id, text])
    LibpdServer.instance_note_on.connect(func(id, ch, pitch, vel): got["note"] = [id, ch, pitch, vel])
    LibpdServer.instance_dsp_active.connect(func(id, active): got["active"] = [id, active])
    LibpdServer.debug_push_print(1, "hello pd")
    await get_tree().process_frame
    assert(got.has("print") and got["print"][0] == 1 and got["print"][1] == "hello pd", "print signal not delivered: %s" % got)
    print("TEST3_OK")
    get_tree().quit(0)
```

`test_project/scenes/test_server.tscn`: `Node2D` + that script.

- [ ] **Step 2: Run to verify it fails**

Set `run/main_scene="res://scenes/test_server.tscn"` in `project.godot` (revert after task).
Run: `godot --headless --path ../test_project --quit` (from `extension/`)
Expected: exit non-zero / assertion error — `debug_push_print` doesn't exist yet.

- [ ] **Step 3: Implement `LibpdServer`**

Node subclass; owns one `PdEventRing`; `_process()` pops up to 128 events per frame and emits the matching signal on the main thread. `debug_push_print` pushes a PRINT event. `instance_registered`/`instance_count` back a `std::unordered_set<int64_t>` that later tasks' `LibpdInstance` registers into at `_enter_tree` / `_exit_tree`.

- [ ] **Step 4: Run to verify it passes**

Run: `godot --headless --path ../test_project --quit`
Expected: exit 0, stdout contains `TEST3_OK`.

Restore `run/main_scene` to `boot_check.tscn` after the test passes (or keep test scenes additive — do NOT leave test_server as the main scene).

- [ ] **Step 5: Commit**

```bash
git add extension/ test_project/
git commit -m "feat(extension): LibpdServer singleton with main-thread signal fan-out"
```

---

### Task 4: `LibpdInstance` + worker thread (real pd, dry audio sink)

**Files:**
- Create: `extension/src/libpd_instance.{h,cpp}`, `extension/src/libpd_worker.{h,cpp}`, `extension/src/core/pd_audio_sink.{h,cpp}` (interface + `DrySink` that only counts blocks)
- Create: `test_project/data/test_patch.pd`, `test_project/scripts/test_instance.gd`, `test_project/scenes/test_instance.tscn`
- Modify: `register_types.cpp` (register `LibpdInstance`), `CMakeLists.txt` (add sources; link the real `libpd` static lib here for the first time — if the Task 1 build didn't yet link it, wire it now)

**Interfaces:**
- Produces (Godot-visible, on `LibpdInstance` extends `Node`):
  - `init(samplerate: int = 44100, n_ins: int = 0, n_out: int = 2) -> bool` — false if already initialized. (srate-vs-AudioServer fail-fast arrives in Task 5.)
  - `load_patch(path: String, search_paths: PackedStringArray = []) -> Error` — `OK` or `ERR_FILE_NOT_FOUND`/`ERR_INVALID_DATA`; auto-stops nothing, requires init.
  - `unload_patch() -> Error` — stops dsp first if active.
  - `start_dsp() -> Error`, `stop_dsp() -> Error`
  - `send_pd_message(receiver: String, args: PackedStringArray) -> void`, `set_parameter(path: String, value: float) -> void` (wraps `libpd_message("sig~"...` style: sends `value` to receiver `path`), `send_midi(channel: int, pitch: int, velocity: int) -> void`
  - Properties: `patch_loaded: bool`, `dsp_active: bool`, `samplerate: int`, `instance_id: int`, `debug_blocks_pushed: int`
  - Signal: `failure(code: int, text: String)`
- `LibpdWorker`: private C++; ctor(instance state); `run()`: loop { stop-check → queue drain (execute pd calls) → if dsp enabled: `libpd_process_float(1, nullptr, outbuf)` then `sink->push_block(outbuf, libpd_blocksize(), n_out)` → `std::this_thread::sleep_for` pacing to `blocksize*1.05/srate`. Pacing: compute `next_tick` with steady_clock before the sleep.
- Worker-only invariants: `libpd_new_instance`/`libpd_set_instance`/`libpd_init`/`libpd_init_audio`/`libpd_openfile`/`libpd_closefile`/`libpd_message`/`libpd_finish_message`/`libpd_midi`/`libpd_process_float`/`libpd_free_instance` are called **only** from the worker thread. Async commands: INIT (blocks the main-thread caller on a promise until done → `init()` returns bool), LOAD, UNLOAD, STOP-THREAD, etc.
- `test_patch.pd`:

```
# test_patch.pd — MIDI note 60 gates an 440 Hz sine; prints the note
[osc~ 440]
[(*~)]
[sig~ 0.3]
[out~ 2]
[notein 0 60] -> [(*~ 1)]  (multiplies into (*~))
[notein 0 60] -> [print test_patch]
```

(Write it as a real patch; a plain text description is not a patch — layout: `(notein 0 60)` → `[(*~)]` and `[print test_patch]` in parallel; `[osc~ 440]` → `[(*~)]` → `[sig~ 0.3]` → `[out~]`.)

- [ ] **Step 1: Write the failing headless test**

`test_project/scripts/test_instance.gd`:

```gdscript
extends Node2D

func _ready():
    var inst := LibpdInstance.new()
    add_child(inst)
    assert(inst.init(44100), "init failed")
    assert(inst.init(44100) == false, "double init must fail")
    assert(inst.load_patch("res://data/nope.pd") != Error.OK, "missing patch must fail")
    assert(inst.load_patch("res://data/test_patch.pd") == Error.OK, "load failed")
    assert(inst.patch_loaded)
    var got_note := []
    LibpdServer.instance_print.connect(func(id, text): if "test_patch" in text: got_note.append(text))
    assert(inst.start_dsp() == Error.OK)
    await get_tree().create_timer(0.3).timeout
    inst.send_midi(0, 60, 100)          # main thread, dsp running
    await get_tree().create_timer(1.0).timeout
    assert(got_note.size() > 0, "note print not received: %s" % got_note)
    inst.send_midi(0, 60, 0)
    assert(inst.stop_dsp() == Error.OK)
    assert(inst.unload_patch() == Error.OK)
    inst.queue_free()
    await get_tree().process_frame
    print("TEST4_OK")
    get_tree().quit(0)
```

`test_project/scenes/test_instance.tscn`: `Node2D` + script. (Set as main scene temporarily; restore `boot_check.tscn` at the end.)

- [ ] **Step 2: Run to verify it fails**

Run: `godot --headless --path ../test_project --quit`
Expected: non-zero — `LibpdInstance` class not registered.

- [ ] **Step 3: Implement `LibpdInstance`, `LibpdWorker`, `PdAudioSink`/`DrySink`**

- `PdAudioSink`: abstract `push_block(const float *non_interleaved_out, int blocksize, int n_out)`, `blocks_pushed() -> int`. `DrySink` counts.
- `LibpdInstance::_enter_tree`: register `instance_id` (assigned monotonically) with `LibpdServer::get_singleton()`; `_exit_tree`: deregister + force `stop_dsp()` if active (must join before node freed — do the join synchronously in `_exit_tree`).
- Printhook: copy text into a ring PRINT event (truncate to 63 bytes at a UTF-8 boundary); noteonhook: NOTE_ON event. Hooks capture `instance_id` via `libpd_set_instancedata`.
- Commands executed on the worker: INIT (create pd instance, `libpd_set_instance`, `libpd_init`, `libpd_init_audio(n_ins, n_out, srate)`, set hooks/instancedata, resolve promise), LOAD (`libpd_openfile`, search paths via `libpd_add_to_search_path` before open; resolve Error), UNLOAD (`libpd_closefile`), MEDIUM (message/midi passthrough — fire-and-forget, no promise), STOP-THREAD.
- `send_pd_message(receiver, args)`: `libpd_message(receiver, first_arg_or_empty, n, argv)` + `libpd_finish_message` — all executed as a worker command; `set_parameter` = `send_pd_message(path, [String(value)])` (float sent as string is fine for v1; note in comment).
- [ ] **Step 4: Run to verify it passes**

Run: `godot --headless --path ../test_project --quit`
Expected: exit 0, stdout contains `TEST4_OK`.

- [ ] **Step 5: Commit**

```bash
git add extension/ test_project/
git commit -m "feat(extension): LibpdInstance node with worker-threaded pd + dry sink"
```

---

### Task 5: `AudioStreamGenerator` sink + samplerate fail-fast

**Files:**
- Create: `extension/src/core/pd_audio_sink_generator.{h,cpp}`
- Modify: `extension/src/libpd_instance.{h,cpp}` (use the generator sink; srate fail-fast in `init`), `CMakeLists.txt`
- Test: `test_project/scripts/test_audio.gd`, `test_project/scenes/test_audio.tscn`

**Interfaces:**
- Produces: `class GeneratorSink : public PdAudioSink` — holds a `Ref<AudioStreamGenerator>` (mix_type 32-bit float, `mix_rate` = instance srate, `buffer_length` = smallest power of two ≥ 2×blocksize and divisible by blocksize, default 2048) + an `AudioStreamPlayer` parented to the `LibpdInstance` node; `play_from_thread()` called from the worker; `push_block` fills the generator buffer.
- `LibpdInstance::init` now returns `false` and emits `failure` if `samplerate != AudioServer::get_singleton()->get_mix_rate()` (checked on the main thread before the worker command is queued).

- [ ] **Step 1: Write the failing headless test**

`test_project/scripts/test_audio.gd`:

```gdscript
extends Node2D

func _ready():
    var inst := LibpdInstance.new()
    add_child(inst)
    assert(inst.init(44100), "init failed (mix rate should be 44100)")
    assert(inst.load_patch("res://data/test_patch.pd") == Error.OK)
    assert(inst.start_dsp() == Error.OK)
    await get_tree().create_timer(0.5).timeout
    assert(inst.debug_blocks_pushed > 0, "no blocks pushed: %d" % inst.debug_blocks_pushed)
    var bad_rate := 48000 if AudioServer.get_mix_rate() == 44100 else 44100
    var inst2 := LibpdInstance.new()
    add_child(inst2)
    var failed := false
    inst2.failure.connect(func(code, text): failed = true)
    assert(inst2.init(bad_rate) == false, "mismatched samplerate must fail fast")
    assert(failed, "failure signal not emitted for samplerate mismatch")
    inst.stop_dsp()
    inst.queue_free()
    inst2.queue_free()
    await get_tree().process_frame
    print("TEST5_OK")
    get_tree().quit(0)
```

- [ ] **Step 2: Run to verify it fails**

Temporarily set `run/main_scene="res://scenes/test_audio.tscn"`.
Run: `godot --headless --path ../test_project --quit`
Expected: non-zero — `debug_blocks_pushed` is 0 (DrySink) or fail-fast not implemented.

- [ ] **Step 3: Implement `GeneratorSink` + fail-fast**

Wire `GeneratorSink` as the default sink in `LibpdInstance` (created on the main thread in `init` after the srate check; passed to the worker). `push_block` converts the non-interleaved pd output into the generator's float32 buffer format.

- [ ] **Step 4: Run to verify it passes**

Run: `godot --headless --path ../test_project --quit`
Expected: exit 0, stdout contains `TEST5_OK`.

Restore main scene to `boot_check.tscn`.

- [ ] **Step 5: Manual audio sanity (operator step, macOS)**

Run the test scene in the editor (`godot --path ../test_project` or the user's editor), press the run button, and confirm **audible** sine output when the test patch's note is active (use `send_midi` from a temporary script or the Task 7 UI if already present). If silent: check `AudioServer` mix rate and bus; the headless test passing means blocks flow — silence here is a routing issue, not a data issue.

- [ ] **Step 6: Commit**

```bash
git add extension/ test_project/
git commit -m "feat(extension): AudioStreamGenerator sink via play_from_thread + srate fail-fast"
```

---

### Task 6: Multi-instance stress + teardown

**Files:**
- Create: `test_project/scripts/test_multi.gd`, `test_project/scenes/test_multi.tscn`
- Modify (only if tests expose a bug): `extension/src/libpd_instance.{h,cpp}`, `extension/src/libpd_worker.{h,cpp}`

**Interfaces:**
- Consumes: full `LibpdInstance` API from Task 4/5.
- Produces: no new API — this task pins the concurrency guarantees (Review Focus items 2–4).

- [ ] **Step 1: Write the stress test**

`test_project/scripts/test_multi.gd`:

```gdscript
extends Node2D

func _ready():
    var instances := []
    for i in 4:
        var inst := LibpdInstance.new()
        add_child(inst)
        assert(inst.init(44100), "init %d failed" % i)
        assert(inst.load_patch("res://data/test_patch.pd") == Error.OK)
        assert(inst.start_dsp() == Error.OK)
        instances.append(inst)
    var prints := {}
    LibpdServer.instance_print.connect(func(id, text):
        if "test_patch" in text: prints[id] = prints.get(id, 0) + 1)
    await get_tree().create_timer(2.0).timeout
    assert(instances.size() == 4 and all([i.dsp_active for i in instances]), "not all instances running")
    for i in instances:
        assert(i.debug_blocks_pushed > 0, "instance %d pushed no blocks" % i.instance_id)
        i.send_midi(0, 60, 100)
    await get_tree().create_timer(1.0).timeout
    assert(prints.size() == 4, "expected prints from all 4 instances, got %s" % prints)
    # spawn/kill cycles: 3 rounds of create/init/load/start/stop/free
    for r in 3:
        var extra := LibpdInstance.new()
        add_child(extra)
        assert(extra.init(44100) and extra.load_patch("res://data/test_patch.pd") == Error.OK and extra.start_dsp() == Error.OK)
        await get_tree().create_timer(0.3).timeout
        extra.queue_free()                       # freed WHILE dsp active
        await get_tree().process_frame
    for i in instances:
        i.stop_dsp()
        i.queue_free()
    await get_tree().process_frame
    print("TEST6_OK")
    get_tree().quit(0)
```

- [ ] **Step 2: Run with a hang guard**

Temporarily set `run/main_scene="res://scenes/test_multi.tscn"`.
Run: `timeout 90 godot --headless --path ../test_project --quit`
Expected: exit 0 within 90 s, stdout contains `TEST6_OK`. (Exit code 124 = hang → fix teardown: the `queue_free`-while-dsp-active path must join the worker in `_exit_tree`.)

- [ ] **Step 3: Fix any failures found** (teardown order, ring drops, promise leaks) and re-run Step 2 until green.

- [ ] **Step 4: Run the full suite one more time**

Restore main scene to `boot_check.tscn`; run the Task 2 ctest suite + Tasks 3/4/5 scenes once each (temporarily switching main scene, or run with `--scene`? Use `godot --headless --path ../test_project scenes/test_multi.tscn --quit` if the 4.6 CLI accepts a scene path — verify; otherwise switch main scene per run).
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add extension/ test_project/
git commit -m "test(extension): multi-instance stress + teardown guarantees"
```

---

### Task 7: Test app UI + macOS release export + smoke test

**Files:**
- Create: `test_project/scenes/test.tscn`, `test_project/scripts/test_main.gd`
- Modify: `test_project/project.godot` (main scene → `test.tscn`), create `test_project/export_presets.cfg` (macOS release + Linux + Android presets)
- Modify (bug fixes only if UI testing finds them): `extension/src/*`

**Interfaces:**
- Consumes: `LibpdServer` signals + `LibpdInstance` API (Tasks 3–6).
- Produces: the runnable test app per spec §9, plus a `--smoke` argument mode in `test_main.gd` (the exported app runs the headless self-test and exits when launched with `--smoke`; this works inside release exports without extra files) used by all platform verifications.

- [ ] **Step 1: Implement the test scene**

`test.tscn` + `test_main.gd` exactly per spec §9: Label (title, instance count, samplerate), Buttons Load Patch / Start-Stop DSP / Send Test Note / +Instance / Kill Last, RichTextLabel (last 20 `instance_print` lines, auto-scroll). `+Instance` adds a new `LibpdInstance` that loads the same patch and starts dsp automatically; `Kill Last` frees the most recent. Wire the three `LibpdServer` signals. The initial scene contains one pre-created (but not yet initialized) `LibpdInstance`; "Load Patch" initializes it at `AudioServer.get_mix_rate()` if not already.

- [ ] **Step 2: Implement the `--smoke` argument mode in `test_main.gd`**

When `OS.get_cmdline_args()` contains `--smoke`, `_ready()` skips the UI and runs, then quits:

1. create + init `LibpdInstance` (`AudioServer.get_mix_rate()`),
2. `load_patch("res://data/test_patch.pd")`,
3. `start_dsp()`, await 0.5 s, assert `debug_blocks_pushed > 0`,
4. `send_midi(0, 60, 100)`, await 0.5 s, assert a print arrived,
5. spawn a second instance, run 0.5 s, free it,
6. stop + free first, `print("SMOKE_OK")`, `quit(0)`; any failure → `quit(1)`.

- [ ] **Step 3: Run the UI locally and verify by hand**

Run: `godot --path ../test_project` (editor opens the scene) → Run. Click Load → Start → Send Test Note: audible sine appears and stops with velocity 0; prints visible; +Instance doubles the note; Kill Last removes it without crash. Fix whatever this reveals.

- [ ] **Step 4: Create export presets**

`export_presets.cfg` with three presets: `MacOS` (release, arch arm64, export to `dist/macos/`), `Linux` (release), `Android` (debug + release, arm64-v8a). Keep defaults; verify each preset appears in the editor's export dialog.

- [ ] **Step 5: Export macOS release + run smoke**

Run:
```
godot --headless --path ../test_project --export-release "MacOS"
./dist/macos/<app>/Contents/MacOS/<binary> --headless --smoke
```
(Use the actual export path/binary name from the preset.)
Expected: exit 0, `SMOKE_OK`.

- [ ] **Step 6: Operator step — manual macOS run**

User runs the exported app, plays the buttons, confirms audio + UI per spec §9 success criteria.

- [ ] **Step 7: Commit**

```bash
git add test_project/
git commit -m "feat(test): full test app UI + export presets + smoke self-test"
```

---

### Task 8: Knulli engine + extension builds via Docker

**Files:**
- Create: `engine_build/Dockerfile.knulli`, `engine_build/build-knulli-engine.sh`

**Interfaces:**
- Produces: `bin/godot.linuxbsd.arm64` (Godot 4.6-stable release, native aarch64) and `extension/build/linux/libgodot_libpd.so`.
- Consumes: `build.sh --linux-arm64` (must work with plain aarch64 gcc, no NDK).

- [ ] **Step 1: Write `engine_build/Dockerfile.knulli`**

Mirror `../trackerjolo-v/Dockerfile.knulli` structure: `FROM --platform=linux/arm64 ubuntu:22.04`; apt: `build-essential python3 python3-pip python3-yaml ca-certificates file kmod pkg-config libasound2-dev libfontconfig-dev libfreetype-dev libgl1-mesa-dev libgles-dev`; `pip3 install scons` (or apt `scons` if available); copy `build-knulli-engine.sh`; `WORKDIR /src`; `CMD ["/bin/bash", "/build/build-knulli-engine.sh"]`.

- [ ] **Step 2: Write `build-knulli-engine.sh`**

```
#!/bin/sh
set -e
cd /src
git -C . checkout 4.6-stable 2>/dev/null || true   # engine dir is this repo root
scons platform=linux target=release arch=arm64 dev_build=false -j"$(nproc)"
file bin/godot.linuxbsd.arm64
cd extension && ./build.sh --linux-arm64
file build/linux/libgodot_libpd.so
```

(`build.sh --linux-arm64` implemented here: plain cmake host build, `CMAKE_BUILD_TYPE=Release`; no NDK.)

- [ ] **Step 3: Build the engine at `4.6-stable`**

The engine clone is on `main`; check out `4.6-stable` (in a dedicated step, record the checkout in the commit message) — this keeps the submodule pins valid.

- [ ] **Step 4: Run the Docker build**

```
docker build --platform linux/arm64 -f engine_build/Dockerfile.knulli -t godot-libpd-knulli-builder .
docker run --platform linux/arm64 -v "$PWD":/src godot-libpd-knulli-builder
```
Expected: `bin/godot.linuxbsd.arm64` is `ELF 64-bit LSB pie executable, ARM aarch64`; `extension/build/linux/libgodot_libpd.so` is aarch64.

- [ ] **Step 5: Commit** (scripts only; `bin/` and `extension/build/` gitignored)

```bash
git add engine_build/
git commit -m "build(engine): Knulli aarch64 release engine + linux extension via Docker"
```

---

### Task 9: Knulli packaging + Trimui Brick device verification

**Files:**
- Create: `engine_build/package-knulli.sh`, `dist/` outputs (gitignored)

**Interfaces:**
- Consumes: Task 8 binary + Task 7 Linux export preset + extension linux build.
- Produces: `dist/knulli/<board>/` folder (board ∈ `a133`, `h700`) ready to copy to the device: the exported app (binary with embedded pck, **built from our custom engine binary**), the extension `.so`, `godot.sh` launcher.

- [ ] **Step 1: Install the custom binary as the 4.6.2 linux-arm64 template**

```
mkdir -p ~/.local/share/godot/templates/4.6.2-stable/
cp bin/godot.linuxbsd.arm64 ~/.local/share/godot/templates/4.6.2-stable/Godot_v4.6.2-stable_linux.arm64.release
```

(Linux export = editor copies the template binary and injects the pck, so exporting from this template embeds **our** binary. Note the replacement; keep a backup of the official file in `dist/`.)

- [ ] **Step 2: Export + package**

`engine_build/package-knulli.sh BOARD=a133`:
1. `godot --headless --path test_project --export-release "Linux" dist/knulli/$BOARD/test`
2. `file dist/knulli/$BOARD/test` → must report `aarch64`; if it reports x86-64, set the preset's architecture option to arm64 in `export_presets.cfg` and re-export.
3. verify the extension `.so` is next to the binary where the `.gdextension` expects it (bundled by the GDExtension export plugin — inspect the output),
4. write `godot.sh`: `#!/bin/sh\nexec "$(dirname "$0")/test" "$@"`, `chmod +x`,
5. repeat for `BOARD=h700` (identical content; board label only).

- [ ] **Step 3: Operator step — device test on the Trimui Brick**

User copies `dist/knulli/a133/` to the Brick (e.g. `roms/ports/`), runs `./godot.sh`.
Expected: test app opens, Load → Start → Send Test Note produces **audible sine out of the 3.5 mm jack**, prints appear, +Instance works.

- [ ] **Step 4: glibc fallback ladder (only if Step 3 fails with `GLIBC_x.x not found` or missing-symbol errors)**

1. Rebuild engine with static libc: `scons platform=linux target=release arch=arm64 dev_build=false static_libc=true` (in the same Docker), repackage.
2. If static libc fails, lower the glibc floor in `platform/linux/detect.py` (build flag / env) and rebuild.
3. Re-run device test after each step. Record what worked in `engine_build/README.md`.

- [ ] **Step 5: Commit**

```bash
git add engine_build/
git commit -m "build(knulli): device packaging + launcher; note glibc outcome"
```

---

### Task 10: Android GDExtension `.so` (NDK Docker)

**Files:**
- Create: `engine_build/Dockerfile.android`, `engine_build/build-android-extension.sh`
- Modify: `extension/build.sh` (implement `--android` — cmake with NDK toolchain: `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DGODOT_PLATFORM=android` per godot-cpp 4.5's cmake options; `BUILD_PORTMIDI=OFF`)

**Interfaces:**
- Produces: `extension/build/android-arm64/libgodot_libpd.so` (arm64-v8a).
- Consumes: extension sources (Tasks 1–6).

- [ ] **Step 1: Write the Android Docker + build script**

`FROM --platform=linux/arm64 ubuntu:22.04`; install NDK r26 (download `android-ndk-r26-linux.zip` from `dl.google.com/android/repository/`, unzip to `/opt/android-ndk`); build script runs `./build.sh --android` inside a source mount.

- [ ] **Step 2: Build and verify the `.so`**

Run:
```
docker build --platform linux/arm64 -f engine_build/Dockerfile.android -t godot-libpd-android-builder .
docker run --platform linux/arm64 -v "$PWD":/src godot-libpd-android-builder
```
Then:
```
file extension/build/android-arm64/libgodot_libpd.so    # ELF aarch64
/opt/android-ndk/.../llvm/bin/llvm-readelf -d extension/build/android-arm64/libgodot_libpd.so | head   # from host if NDK installed there, else readelf inside the container: SONAME must be libgodot_libpd.so, no undefined GLIBC symbols
```
Expected: aarch64 shared object with `SONAME: libgodot_libpd.so`.

- [ ] **Step 3: Operator step — Android verification (user)**

User places `extension/build/android-arm64/libgodot_libpd.so` where `godot-libpd.gdextension`'s `android.arm64` entry points (the gdextension ships with the project; the `.so` is bundled by the export), exports the test project for Android with the official 4.6.2 templates + user's SDK, sideloads the debug build, and runs the success criteria: audio plays, prints appear, +Instance works. The `--smoke` self-test (Task 7) can be triggered on-device if the user wants automated proof.

- [ ] **Step 4: Commit**

```bash
git add engine_build/ extension/build.sh
git commit -m "build(android): NDK arm64-v8a extension build via Docker"
```

---

## Final definition of done (all of spec §9 success criteria)

- [ ] macOS: exported release app passes `--smoke` headless; manual run: audible sine, prints, +Instance/Kill Last.
- [ ] Android: `libgodot_libpd.so` (arm64-v8a) built; user's exported app passes the same criteria on-device.
- [ ] Trimui Brick (A133): `dist/knulli/a133/godot.sh` launches the app; audible sine out of the jack; prints; multi-instance works.
- [ ] All unit tests (Task 2) + headless integration tests (Tasks 3–6) green on macOS.
- [ ] `docs/` updated: extension README (build instructions per platform), engine_build README (Knulli outcomes incl. any glibc fallback used).

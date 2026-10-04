# v2 M6 — AAudio Native Audio (Android / RG DS) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Route libpd audio on **Android (RG DS)** through **AAudio** using the
existing M5 `NativeAudio` mix-down architecture: **N synth `LibpdInstance`
workers** (one pinned audio thread each) feed their stereo pairs into
`MixInputRing`s; a dedicated **mix-down `LibpdInstance`** (16-in/2-out) renders
in the **AAudio data callback**; the mixed stereo goes to the device. Replace
the Android a1 `AudioStreamGenerator` fallback with the native path.

**Architecture:** *Reuses the M5 revised model verbatim.* One dedicated audio
thread per instance (the a1 worker model, `libpd_set_instance` once per
thread) + a mix-down instance rendered on the **AAudio callback thread** (its
one thread, `set_instance(mix)` once). The only M6 change vs M5 is the
**`AudioPort` backend**: a new `AAudioPort` (implements `AudioPort` with the
AAudio C API) replaces `PortAudioPort` on Android. The `NativeAudio` render
loop, the `MixInputRing`s, the kick transport, and the callback-driven kick
are all **unchanged** — they are `AudioPort`-agnostic.

**Tech Stack:** godot-cpp GDExtension, C++17, **AAudio C API** (NDK r25 sysroot
`libaaudio.so`, API 26-era surface), libpd-static (`libpd-multi.a`, PD_MULTI
ON), CMake/ctest, NDK r25 (`aarch64-linux-android29-clang`).

**Spec:** `docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md`
(status: M5 COMPLETE; §5 lists M6 = Android AAudio).

## Done (do not re-do)

- **M5 T1–T9** — the entire native-audio mix path is complete + verified on
  macOS: `AudioPort`/`NullPort`/`mix_block`, `PortAudioPort` (CoreAudio/ALSA),
  `MixInputRing` (kick transport + K-blocks-per-kick), `NativeAudio`
  (mix-down render-in-callback + `mix_render_lock_` + `closing_` + `wakeup`),
  worker roles (SYNTH ring sink / MIXER control-only / ANDROID a1 fallback),
  server `audio_*` API + `set_mixer`, CMake `NATIVE_AUDIO` (ON macOS/Linux,
  OFF Android), test app. All 16 host test binaries pass. (Commits through
  `a60122a13c`.)
- **T1 recon — AAudio spike on RG DS (this plan)** (commit pending):
  `spike/aaudio/aaudio_spike.c` — standalone NDK arm64 binary, AAudio C API,
  440 Hz stereo sine, 6 s. **Verified on RG DS (Android 14, RK3568):**
  `libaaudio.so` links + loads; stream opens (rate=44100 ch=2 format=PCM
  float state=OPEN); the data callback delivers a **fixed 256-frame count**
  (via `AAudioStreamBuilder_setFramesPerDataCallback`); ~44.1k frames/s over
  6 s; `AAudioStream_requestStop`/`AAudioStream_close` return OK. The NDK r25
  AAudio surface is the **API 26-era C API**: `AAUDIO_FORMAT_PCM_FLOAT`,
  `AAudioStreamBuilder_setDataCallback`, `AAudioStreamBuilder_openStream`,
  `AAudioStreamBuilder_delete`, `AAudioStream_requestStart/Stop`,
  `AAudioStream_close`. Default negotiated buffer is large (~3544 frames ≈
  80 ms) → the port must request a small capacity.

## Global Constraints

- **THE threading invariant (unchanged from M5):** each libpd instance's
  `libpd_process_float` runs on **exactly one thread** that called
  `libpd_set_instance(it)` **exactly once**. No thread ever switches `pd_this`
  between instances. Synth workers = one thread each; the mix-down renders on
  the **AAudio callback thread** (its one thread, `set_instance(mix)` once,
  on the main thread per Option A). (Spec §4.)
- **AAudio data callback is real-time:** no allocation, no file I/O, no
  network, no mutex, no sleep, no stop/close, no `AAudioStream_read/write`.
  Only `AAudioStream_get*()` + `AAudio_convertResultToText()` are allowed.
  Our `NativeAudio::render_block` (gather + mix render + kick/wait) satisfies
  this on macOS via PortAudio; it must equally be safe on the AAudio callback.
  **The AAudio callback must deliver a fixed `framesPerDataCallback`**
  (== the stream blocksize) so `render_block`'s fixed-buffer gather is valid.
- **Block-size contract (unchanged):** `libpd_blocksize()==64`; the stream
  blocksize MUST be a multiple of 64; default 256. `AAudioPort::open` rejects
  `blocksize % 64 != 0`.
- **Output-only for M6:** `AAudioPort::supports_input()` returns false;
  `n_ins` is 0 (mix inputs come from the worker rings, not the device). Audio
  capture (`adc~`) is a separate milestone.
- **Buffer size:** request a small `AAudioStreamBuilder_setBufferCapacityInFrames`
  (≈ 4 × blocksize, e.g. 1024 for blocksize 256) so latency stays low; the
  system may negotiate larger — log the negotiated `getBufferSizeInFrames`.
- **Device enumeration is a non-goal for M6:** `AAudioPort::list_outputs()`
  returns the single default output device; `list_inputs()` is empty.
- **NDK r25 AAudio C API (API 26-era):** use the exact exported symbols
  (see T1 recon): `AAudio_createStreamBuilder`, `AAudioStreamBuilder_set{
  SampleRate,ChannelCount,Format,SharingMode,PerformanceMode,Direction,
  BufferCapacityInFrames,FramesPerDataCallback,DataCallback}`,
  `AAudioStreamBuilder_openStream`, `AAudioStreamBuilder_delete`,
  `AAudioStream_requestStart/Stop`, `AAudioStream_close`,
  `AAudioStream_get{SampleRate,ChannelCount,Format,BufferSizeInFrames,
  BufferCapacityInFrames,State}`, `AAudio_convertResultToText`.
  Format = `AAUDIO_FORMAT_PCM_FLOAT` (float32). Direction =
  `AAUDIO_DIRECTION_OUTPUT`. Sharing = `AAUDIO_SHARING_MODE_SHARED`.
  Performance = `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`.
- **CMake:** `NATIVE_AUDIO` becomes **ON on Android** (arm64-v8a). The
  `AAudioPort` compiles only on Android; `PortAudioPort` compiles only on
  macOS/Linux. Android links `aaudio` (NDK sysroot `libaaudio.so`). PortAudio
  is **not** built on Android.
- **minSdk 29** (AAudio requires API 28+; the app already targets 29).
- **Subagent dispatches MUST set `timeoutMs: 7200000`** (2 h). HARD
  VERIFICATION RULE for on-device claims.
- **RG DS specifics:** serial `56cda52938004166`; NDK
  `~/Library/Android/sdk/ndk/25.1.8937393`; JDK
  `/opt/homebrew/Cellar/openjdk@17/17.0.19/libexec/openjdk.jdk/Contents/Home`;
  dev keystore `dist/android/godot-libpd-dev.keystore`; `apksigner` at
  `$SDK/build-tools/<ver>/apksigner`; after rebuilding the `.so`, copy it to
  `test_project/android/build/libs/debug/arm64-v8a/`; the engine uses
  `-fvisibility=hidden` on Android; C-level `printf` is NOT routed to logcat
  (use `__android_log_print`/`pd_dbg` for on-device diagnostics).

## Review Focus

- **No instance-switching** — the AAudio callback binds the mix-down
  **exactly once** (Option A: created on the main thread, adopted by the
  callback). A second/switching `libpd_set_instance` on the callback thread is
  a Critical finding. (Task 3.)
- **Fixed callback frame count** — `AAudioPort` MUST set
  `AAudioStreamBuilder_setFramesPerDataCallback(blocksize)` so the callback
  always delivers `blocksize` frames (the `render_block` gather assumes a
  fixed window). A variable frame count is a Critical finding. (Task 2.)
- **Real-time callback discipline** — the AAudio callback path must do no
  allocation / I/O / unbounded lock. The kick/wait transport (M5 T9) must be
  safe on the AAudio callback (it is the same `render_block` proven on
  PortAudio). (Task 3.)
- **NATIVE_AUDIO on Android** — the CMake must compile `AAudioPort` (not
  `PortAudioPort`) on Android, enable `NATIVE_AUDIO`, link `aaudio`, and NOT
  build PortAudio. The a1 `AudioStreamGenerator` fallback is retired on
  Android. (Task 4.)
- **On-device proof** — the RG DS must actually play the mixed multi-synth
  output (audible), with no crash, no crackle, and the callback running at the
  negotiated rate. (Task 5.)

---

### Task 2: `AAudioPort` — `AudioPort` over the AAudio C API

The Android audio backend. Mirrors `PortAudioPort` but uses the AAudio C API.
Output-only, float32, fixed callback frame count. Host-compilable for syntax
(device parts are Android-only; guard with `#ifdef __ANDROID__`).

**Files:**
- Create: `extension/src/core/aaudio_port.h`
- Create: `extension/src/core/aaudio_port.cpp`
- Test: `extension/tests/aaudio_port_tests.cpp` (Android-only; compiles the
  port + checks `supports_input()==false`, `list_inputs()` empty, and the
  `blocksize % 64` rejection path on a device-open attempt)

**Interfaces:**
- Consumes: `AudioPort` (`core/audio_port.h`).
- Produces:
  - `namespace godot_libpd { class AAudioPort : public AudioPort { public:
      AAudioPort(); ~AAudioPort() override;
      int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) override;
      void close() override; bool is_open() const override;
      std::vector<AudioDeviceInfo> list_inputs() const override;
      std::vector<AudioDeviceInfo> list_outputs() const override;
      double output_latency_ms() const override;
      double input_latency_ms() const override;
      bool supports_input() const override;
    private:
      static aaudio_data_callback_result_t aa_cb(AAudioStream*, void* userData, void* audioData, int32_t numFrames);
      AAudioStream *stream_ = nullptr;
      AAudioStreamBuilder *builder_ = nullptr;
      bool open_ = false;
      double output_latency_ms_ = 0.0;
  }; }`
  - `open()`: reject `p_blocksize % 64 != 0` and `p_n_ins != 0` (output-only)
    with a nonzero code. Create the builder; set sample rate, channel count
    (`p_n_out`), format `AAUDIO_FORMAT_PCM_FLOAT`, sharing `SHARED`,
    performance `LOW_LATENCY`, direction `OUTPUT`, buffer capacity
    `4 * p_blocksize`, **`setFramesPerDataCallback(p_blocksize)`**, and the
    data callback (`aa_cb`, userData=this). `openStream`, then
    `requestStart`. On success read back the negotiated sample rate/channel
    count/`getBufferSizeInFrames` and compute `output_latency_ms_`
    (`bufferSizeInFrames * 1000.0 / sampleRate`). On any failure, clean up
    (close/delete what was created) and return nonzero.
  - `aa_cb()`: cast `audioData` to `float*`; call
    `self->render_(nullptr, out, numFrames)`; return
    `AAUDIO_CALLBACK_RESULT_CONTINUE`. (The callback never reads device input;
    `p_dev_in` is nullptr — output-only.)
  - `close()`: `requestStop` + `AAudioStream_close` + `StreamBuilder_delete`
    (idempotent; null the pointers). `is_open()` returns `open_`.
  - `list_outputs()`: one `AudioDeviceInfo{ index 0, name "AAudio default
    output", max_in 0, max_out 2 }`. `list_inputs()`: empty.
  - `supports_input()`: false. `input_latency_ms()`: 0.0.

- [ ] **Step 1: Write the failing test**

`extension/tests/aaudio_port_tests.cpp` (CHECK/main style, mirror
`audio_portaudio_tests.cpp`): guard the whole file with
`#if defined(__ANDROID__)`. Tests:
  1. `AAudioPort p; CHECK(p.supports_input() == false);`
  2. `CHECK(p.list_inputs().empty());`
  3. `CHECK(p.list_outputs().size() == 1);`
  4. `CHECK(p.open(0, 2, 44100, 100) != 0);` (100 % 64 != 0 → reject)
  5. `CHECK(p.open(1, 2, 44100, 256) != 0);` (n_ins != 0 → reject, output-only)
  6. `CHECK(p.open(0, 2, 44100, 256) == 0);` then
     `CHECK(p.is_open());` `p.close();` `CHECK(!p.is_open());`
     (device open on the RG DS; if the device is unavailable this may fail —
     in that case mark it SKIP, not FAIL.)

- [ ] **Step 2: Build for the RG DS and verify it fails**

Build the test via the Android CMake test target (or a standalone NDK
compile); expected: FAIL (no `AAudioPort`).

- [ ] **Step 3: Implement `AAudioPort`**

`aaudio_port.{h,cpp}`. `#include <AAudio/AAudio.h>` inside the `.cpp` (guarded
by `#ifdef __ANDROID__`); the header only forward-declares / includes
`audio_port.h` so it is portable. Implement per the interface above. Keep the
callback allocation-free.

- [ ] **Step 4: Build + run on the RG DS; verify it passes**

Rebuild + run the test on the RG DS; expected: PASS (6/6, or 5 PASS + 1 SKIP
if the device is unavailable at that moment).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/aaudio_port.h extension/src/core/aaudio_port.cpp \
        extension/tests/aaudio_port_tests.cpp
git commit -m "native audio: AAudioPort AudioPort over the AAudio C API (M6 T2)"
```

---

### Task 3: Server port selection — `AAudioPort` on Android, `PortAudioPort` elsewhere

Wire the backend choice into the server so Android uses `AAudioPort` and
macOS/Linux use `PortAudioPort`. The `NativeAudio` render loop is unchanged
(it is `AudioPort`-agnostic). This is the task that makes the M5 mix path run
on the AAudio callback.

**Files:**
- Modify: `extension/src/libpd_server.{h,cpp}` (where the `AudioPort` is
  created for `NativeAudio::open`)
- Test: `extension/tests/server_audio_tests.cpp` (add a port-selection
  assertion if feasible on host; otherwise document the Android path)

**Interfaces:**
- Consumes: `AAudioPort`, `PortAudioPort`, `NativeAudio`.
- Produces: a factory/selection that returns an `AAudioPort` on
  `#ifdef __ANDROID__` and a `PortAudioPort` otherwise, passed to
  `NativeAudio::open()`. The mix-down binding (`set_mixer`, Option A:
  created on the main thread, adopted by the callback) is unchanged.

- [ ] **Step 1: Confirm the current port-creation site**

Locate where `libpd_server.cpp` constructs the `AudioPort` for
`NativeAudio::open` (the `PortAudioPort`). Read it; note the include +
construction.

- [ ] **Step 2: Add the platform selection**

Introduce `make_audio_port()` (or inline `#ifdef __ANDROID__`) that returns a
`std::unique_ptr<AudioPort>`: `AAudioPort` on Android, `PortAudioPort`
otherwise. Update `NativeAudio::open` to take the selected port. Keep the
no-device `NullPort` fallback path intact (headless/tests).

- [ ] **Step 3: Verify the macOS build is unchanged**

Rebuild macOS (`build.sh --macos`) + run all 16 host test binaries; expected:
PASS (the selection returns `PortAudioPort` on macOS, so behavior is
identical).

- [ ] **Step 4: Commit**

```bash
git add extension/src/libpd_server.h extension/src/libpd_server.cpp \
        extension/tests/server_audio_tests.cpp
git commit -m "native audio: select AAudioPort on Android / PortAudioPort elsewhere (M6 T3)"
```

---

### Task 4: CMake — `NATIVE_AUDIO` ON on Android + `AAudioPort` + link `aaudio`

Enable the native mix path on Android (arm64-v8a): compile `AAudioPort` (not
`PortAudioPort`), define `NATIVE_AUDIO`, link the NDK `aaudio` library, and
retire the a1 `AudioStreamGenerator` fallback on Android.

**Files:**
- Modify: `extension/CMakeLists.txt`

**Interfaces:**
- Consumes: the existing `NATIVE_AUDIO` block + the Android block.
- Produces: `NATIVE_AUDIO=ON` for Android arm64-v8a; `aaudio_port.cpp`
  compiled into `godot_libpd` on Android; `aaudio` linked on Android;
  `portaudio_lib` NOT built/linked on Android.

- [ ] **Step 1: Read the current NATIVE_AUDIO + Android CMake**

Read `extension/CMakeLists.txt` lines around the `NATIVE_AUDIO` block (≈95–215)
+ the Android block. Note: `NATIVE_AUDIO` is currently OFF on Android;
`portaudio_port.cpp` is the only backend compiled.

- [ ] **Step 2: Enable NATIVE_AUDIO on Android + add AAudioPort**

Change the `NATIVE_AUDIO` gate so it is ON for Android arm64-v8a too. In the
`NATIVE_AUDIO` source list, compile `src/core/aaudio_port.cpp` on Android and
`src/core/portaudio_port.cpp` on macOS/Linux. On Android, link `aaudio`
(`target_link_libraries(godot_libpd PRIVATE aaudio)`) instead of
`portaudio_lib`. Do NOT build `portaudio_lib` on Android. Keep the
`NATIVE_AUDIO` compile definition.

- [ ] **Step 3: Retire the Android a1 fallback (or gate it off)**

The synth/mixer worker currently falls back to the a1
`AudioStreamGenerator` on Android. With `NATIVE_AUDIO` ON, Android uses the
native mix path. Ensure the worker role selection picks SYNTH/MIXER (not the
ANDROID a1 role) on Android now that native audio is available. (If the a1
code is still referenced, keep it compiled but unselected; do not delete it
yet — M6 is the first Android native path, keep the fallback for
roll-back.)

- [ ] **Step 4: Build the Android `.so`**

`build.sh --android` (arm64-v8a). Expected: the `.so` builds with
`NATIVE_AUDIO` + `AAudioPort`, no PortAudio, links `aaudio`.

- [ ] **Step 5: Commit**

```bash
git add extension/CMakeLists.txt
git commit -m "native audio: CMake NATIVE_AUDIO on Android + AAudioPort + link aaudio (M6 T4)"
```

---

### Task 5: Test app native mode on Android + RG DS on-device verify

Run the `test_native_mix` app (8 synth + 1 mix-down) on the RG DS through the
AAudio backend; verify audible multi-synth output, no crash, no crackle, and a
stable callback.

**Files:**
- Modify (if needed): `test_project/scripts/test_native_mix.gd` (ensure the
  native-mix path is selected on Android; the `NM_SYNTHS` env var still
  applies)
- Reuse: `test_project/data/mixdown_16.pd` + `test_project/data/synth.pd`
  (the M5 patches)

**Steps:**
- [ ] **Step 1: Build the APK**

Rebuild the Android `.so` (T4), copy it to
`test_project/android/build/libs/debug/arm64-v8a/`, then
`gradlew assembleStandardDebug`. Sign with the dev keystore + `apksigner`.

- [ ] **Step 2: Deploy + run the native-mix scene on the RG DS**

`adb install` the APK; launch the `test_native_mix` scene (foreground — the
RG DS stalls backgrounded apps at init, per M4). Set `NM_SYNTHS=8` (via
`command_line/extra_args`, since `am start -- args` does not reach the
engine). Capture logcat.

- [ ] **Step 3: Verify on-device (HARD VERIFICATION RULE)**

Confirm via logcat + the user's ears: (a) `NativeAudio::open` succeeds on the
AAudio backend (negotiated rate/channels logged); (b) 8 SYNTH workers start +
the mix-down binds (Option A); (c) the AAudio callback runs at the negotiated
rate (no xruns/errors); (d) **audible** multi-synth output (the user hears the
mixed chord); (e) no crash over a ~60 s soak. Capture the exact logcat lines
as evidence.

- [ ] **Step 4: Soak + clean shutdown**

Let it run ~60 s; then stop the app; confirm a clean shutdown (AAudio
stream stop/close, worker joins, no hang). Capture logcat.

- [ ] **Step 5: Update docs + close M6**

Update `extension/README.md` (native audio now on Android) +
`docs/android-build.md` (AAudio backend). Flip the M5/M6 spec status to
"M6 COMPLETE". Leave the RG DS clean.

- [ ] **Step 6: Commit**

```bash
git add test_project/scripts/test_native_mix.gd extension/README.md \
        docs/android-build.md docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md
git commit -m "native audio: AAudio on Android verified on RG DS (M6 T5) + docs"
```

---

## Self-Review (writing-plans checklist)

- [x] Every task has files + a testable step (host test for T2; build/verify
  for T3/T4; on-device verify for T5).
- [x] The threading invariant + fixed callback frame count are called out as
  Critical review focus.
- [x] Real-time callback discipline is enforced (no allocation/I/O/unbounded
  lock on the AAudio callback).
- [x] The NDK r25 AAudio C API surface is pinned to the exact exported
  symbols (T1 recon).
- [x] CMake changes are scoped (NATIVE_AUDIO ON Android, AAudioPort, link
  aaudio, no PortAudio on Android).
- [x] On-device proof is required (audible + logcat evidence), per the HARD
  VERIFICATION RULE.
- [x] Roll-back is preserved (the a1 fallback is kept compiled but
  unselected until T5 proves the native path).

## Deferred / non-goals for M6

- Audio capture (`adc~`) on Android — separate milestone.
- AAudio device enumeration + device selection UI — non-goal (default
  output only).
- Exclusive (XRUN-free) AAudio mode — non-goal (SHARED is fine for M6).
- OpenSL ES fallback — superseded by AAudio (the spec's "OpenSL v1" item).
- iOS/Windows MIDI, minSdk < 26, PortMIDI deletion, M4 non-goals — unchanged.

# v2 M5 — Native Audio Backend (macOS + Linux) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route libpd audio through a native PortAudio stream on macOS (CoreAudio) and Linux (ALSA/Knulli), so a `[adc~]`-fed patch renders to speakers at low latency; retire the a1 `AudioStreamGenerator` sink on these two platforms (Android keeps a1 until M6).

**Architecture:** Approach A (spec §4): one PortAudio real-time callback renders **all** registered instances (`libpd_process_float` per instance, summed + clamped into the device output); per-instance worker threads become pure control threads; a coarse `render_lock` serializes openfile/init/free with rendering. A platform-free `AudioPort` seam (`PortAudioPort` + `NullPort`) makes the mix + per-instance render loop testable on any host without a device. The worker's a1 DSP loop is gated by a `native_audio` config flag so Android (a1) is untouched.

**Tech Stack:** godot-cpp GDExtension, C++17, **vendored PortAudio 19.7.0** (`extension/thirdparty/libpd/pure-data/portaudio/portaudio/`), libpd-multi (`libpd_static`, PD_MULTI ON), CMake/ctest.

**Spec:** `docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md`

## Global Constraints

- **Block-size contract:** `libpd_blocksize() == 64`; the device frame buffer MUST be a multiple of 64. `ticks = frames / 64` per `libpd_process_float` call. Default blocksize **256** (~5.8 ms @ 44.1 kHz). (Spec §6.1.)
- **No resampling:** an instance's `samplerate` MUST equal the native stream's `samplerate`; mismatch fails fast (no resampler). (Spec §6.1.)
- **`pd_this` is thread-local** (spike-confirmed): the audio callback MUST `libpd_set_instance(slot.pd)` for each instance before `libpd_process_float`. A naive cross-thread `process_float` segfaults in pd's `sys_lock` otherwise. (Spec §4.5.)
- **Multi-instance = one device, explicit mix:** all instances render into their own buffer; outputs are summed into `dev_out` and clamped to `[-1, 1]`. (Spec §6.3.)
- **openfile RT-safety (M5 default):** a coarse `render_lock` in `NativeAudio`; the audio thread holds it for the whole block; control ops (INIT/LOAD/UNLOAD/STOP) wrap their `libpd_*` calls in the same lock → a brief audio stall while loading. (Spec §6.2.)
- **No-device fallback:** `NullPort` (counts frames, reports zero latency) preserves headless tests + Knulli CI. (Spec §6.4.)
- **Thread invariants (inherited):** all `libpd_*` on a single thread per instance (control thread for setup, audio thread for render, **serialized by `render_lock`**); Godot signals on the main thread only. Output hooks fire on the audio thread → `midi_out.push()` must stay non-allocating (it is: `MidiOutputQueue` is a lock-free-ish bounded queue).
- **Platform split:** `NATIVE_AUDIO` is ON for macOS + Linux, OFF for Android. The worker's a1 DSP loop runs only when `native_audio == false` (Android). `PdAudioSink`/`GeneratorSink` stay for Android.
- **Input object is `adc~`** (1-indexed); there is no `audioin~` class in this pd build. Device capture feeds `st_soundin`; each instance's `adc~` reads its channels. (Spec §2.)
- Submodules unchanged. `libpd_static`, `godot-cpp`, PortMIDI/RtMidi wiring unchanged.
- Subagent timeouts 7200000 ms (slow model endpoint). HARD VERIFICATION RULE: every on-device claim needs a real command + pasted output.
- PortAudio build notes (spike-confirmed): CoreAudio needs `-framework AudioToolbox` (not just CoreAudio) + `-framework CoreFoundation` + `-framework CoreServices`; `-DPA_USE_COREAUDIO=1` (macOS) so `pa_unix_hostapis.c` registers it. Linux ALSA: `-DPA_USE_ALSA=1`, link `asound`.
- The built `spike/native_audio/native_audio_spike` binary is gitignored; the spike sources (`spike.c`, `build.sh`, `*.pd`) are the reproducible reference for the callback + `libpd_process_float` contract.

## Review Focus

- **`pd_this` thread-locality** — the callback must set the instance before every `process_float`; forgetting it segfaults in `sys_lock`, not "fails gracefully" — Task 3 test (render 1 tick per instance; a wrong/missing `set_instance` produces a crash or misattributed audio).
- **Block-size not a multiple of 64** — `audio_open(100, …)` must fail (reject or clamp) rather than call `libpd_process_float` with a fractional tick count — Task 5 test.
- **Samplerate mismatch** — an instance `init(48000)` while the stream is 44100 must fail fast (no resample) — Task 5 test.
- **Multi-instance mix overflow** — two full-scale (+1.0) instances summed → must clamp to 1.0, not wrap to negative / overflow — Task 1 test (`mix_block`).
- **Control op while rendering (openfile stall)** — `load_patch` on a running instance must not corrupt a concurrent render (the `render_lock` serializes); assert no crash + audio resumes — Task 4 test (load a patch N times while the NullPort is rendering).
- **Instance removed while rendering** — `unregister_instance` (node exit) while the callback is mid-iteration must not read a freed `RenderSlot` (the slot list snapshot is taken under lock) — Task 3 test.

---

### Task 1: `AudioPort` abstraction + `NullPort` + pure `mix_block`

The platform-free seam. `AudioPort` abstracts a device; `NullPort` simulates an audio thread for host tests; `mix_block` is the pure sum+clamp helper. This task is 100% host-testable with no device and no libpd.

**Files:**
- Create: `extension/src/core/audio_port.h`
- Create: `extension/src/core/null_port.h` (header-only, for tests)
- Test: `extension/tests/audio_port_tests.cpp`

**Interfaces:**
- Consumes: nothing (foundational).
- Produces (for Tasks 2/3/5):
  - `namespace godot_libpd {`
  - `struct AudioDeviceInfo { int index = -1; std::string name; int max_in = 0; int max_out = 0; };` (POD; the server wraps into `Array`.)
  - `typedef void (*RenderFn)(float *p_dev_in, float *p_dev_out, int p_frames);` — the per-block render callback (the audio-thread body). `p_dev_in` has `frames * n_ins` floats (null when `n_ins == 0`); `p_dev_out` is the caller-allocated, zero-initialized scratch that the callback must fill with `frames * n_out` floats.
  - `class AudioPort { public: virtual ~AudioPort() = default; virtual int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) = 0; virtual void close() = 0; virtual bool is_open() const = 0; virtual std::vector<AudioDeviceInfo> list_inputs() const = 0; virtual std::vector<AudioDeviceInfo> list_outputs() const = 0; virtual double output_latency_ms() const = 0; virtual double input_latency_ms() const = 0; virtual bool supports_input() const = 0; void set_render_callback(RenderFn p_fn) { render = p_fn; } protected: RenderFn render = nullptr; };`
  - `void mix_block(float *p_out, int p_out_ch, const float *const *p_inputs, int p_n_inputs, int p_frames);` — sums `p_n_inputs` interleaved buffers (each `p_frames * p_out_ch`) into `p_out`, clamps to `[-1, 1]`. `p_out` is `p_frames * p_out_ch`.
  - `class NullPort : public AudioPort` — `open()` succeeds for any sane args; `list_outputs()` returns one device `{index 0, name "Null", max_in 0, max_out 2}`; `list_inputs()` returns one `{index 0, name "Null In", max_in 1, max_out 0}`; latencies = `blocksize / samplerate * 1000.0` (or 0 if not open); `supports_input() == true`. While open, a background thread calls `render(zeroed_in, scratch_out, blocksize)` at real-time pace (a `std::thread` + `std::chrono` sleep of `blocksize/samplerate`), incrementing `frames_rendered()` each tick and reading peak into `last_out_peak()`. `close()` joins the thread. `NullPort` is the device-free driver for the full `NativeAudio` loop in Task 3/6.

- [ ] **Step 1: Write the failing tests for `mix_block` + `NullPort`**

Create `extension/tests/audio_port_tests.cpp` (a `main()` with `CHECK(...)` helpers, matching the existing test style — no gtest; process exit 0 on success, 1 on any failed CHECK). Write:

1. **mix single input** — two-channel, 2 frames, one input `{0.25, -0.25, 0.5, 0.0}` → `mix_block` → `out == {0.25, -0.25, 0.5, 0.0}`.
2. **mix two inputs sums** — input A `{0.5, 0.0}`, input B `{0.25, -0.5}` (1 frame, 2 ch) → `out == {0.75, -0.5}`.
3. **mix clamps to 1.0** — input A `{1.0}`, input B `{0.5}` (1 frame, 1 ch) → `out == {1.0}` (not 1.5, not wrapped). Negative clamp: A `{-1.0}`, B `{-0.5}` → `out == {-1.0}`.
4. **mix zero inputs** — `p_n_inputs == 0`, pre-filled `out` → `out` stays as-is (mix writes nothing) OR is zeroed — pick zero and assert `out == {0,0}` (decide in the impl; test pins it).
5. **NullPort open + render** — `NullPort port; port.set_render_callback(fn)` where `fn` writes a constant `0.5` into every `p_dev_out` sample; `CHECK(port.open(1, 2, 44100, 256) == 0)`; wait ~120 ms; `CHECK(port.frames_rendered() > 0)` and `CHECK(port.last_out_peak() > 0.4f)`; `port.close()`.
6. **NullPort blocksize honored** — `open(0, 2, 44100, 128)`; the callback records `p_frames`; wait ~120 ms; `CHECK(recorded_frames == 128)`.

- [ ] **Step 2: Run test to verify it fails**

Run: build the new test target (Task 6 adds the CMake wiring; for this step, compile directly):
`c++ -std=c++17 -Iextension/src extension/tests/audio_port_tests.cpp -o /tmp/audio_port_tests -pthread && /tmp/audio_port_tests`
Expected: FAIL (no `AudioPort`/`NullPort`/`mix_block`).

- [ ] **Step 3: Implement `mix_block` + `AudioPort` + `NullPort`**

`extension/src/core/audio_port.h`: the `AudioPort` base, `AudioDeviceInfo`, `RenderFn`, and `mix_block` (defined inline in the header so tests + the extension share it). `mix_block`: nested loop over `frames`, then `out_ch`: `acc = sum over inputs[i][f*out_ch+c]`; `out[f*out_ch+c] = clamp(acc, -1.0f, 1.0f)`; if `p_n_inputs == 0`, write 0.

`extension/src/core/null_port.h`: `NullPort` (header-only). `open()` validates `blocksize % 64 == 0` (else return `-1`), starts a `std::thread` that loops while `open_`: allocates scratch `dev_in` (if `n_ins`) + `dev_out` (zeroed), calls `render(...)`, bumps `frames_rendered_` (atomic), tracks `last_out_peak_`, sleeps `blocksize/samplerate` seconds. `close()` sets `open_ = false`, joins.

- [ ] **Step 4: Run test to verify it passes**

Run: `c++ -std=c++17 -Iextension/src extension/tests/audio_port_tests.cpp -o /tmp/audio_port_tests -pthread && /tmp/audio_port_tests`
Expected: PASS (6/6).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/audio_port.h extension/src/core/null_port.h extension/tests/audio_port_tests.cpp
git commit -m "native audio: AudioPort abstraction + NullPort + mix_block (M5 T1)"
```

---

### Task 2: `PortAudioPort` (CoreAudio / ALSA) — device enumeration, stream, latency

The real backend. Wraps PortAudio: enumerates devices, opens a full-duplex stream with a C callback trampoline that dispatches to the `RenderFn`, and reports latencies from `PaStreamInfo`.

**Files:**
- Create: `extension/src/core/portaudio_port.h`
- Create: `extension/src/core/portaudio_port.cpp`

**Interfaces:**
- Consumes: `AudioPort` (Task 1), `RenderFn`. The PortAudio headers (`thirdparty/libpd/pure-data/portaudio/portaudio/include/portaudio.h`) and the PortAudio sources (linked in Task 6).
- Produces (for Task 3):
  - `class PortAudioPort : public AudioPort { public: PortAudioPort(); ~PortAudioPort(); int open(int, int, int, int) override; void close() override; bool is_open() const override; std::vector<AudioDeviceInfo> list_inputs() const override; std::vector<AudioDeviceInfo> list_outputs() const override; double output_latency_ms() const override; double input_latency_ms() const override; bool supports_input() const override; };`
  - A **static** C callback `int pa_callback(void *out, const void *in, unsigned long frames, const PaStreamCallbackTimeInfo*, unsigned int flags, void *userData)` that zero-inits `out` if needed, then calls `static_cast<PortAudioPort *>(userData)->render` (the `RenderFn`) with the raw device buffers and returns `paContinue`. (The `render` call is what Task 3's `NativeAudio` body plugs in — it reads/writes the raw PortAudio buffers, so `PortAudioPort` stays agnostic to libpd.)

- [ ] **Step 1: Write the failing test (host; graceful no-device)**

Add to a new `extension/tests/audio_portaudio_tests.cpp`:
1. **Init + enum** — `PortAudioPort port;` `CHECK(port.list_outputs().size() >= 1)` on macOS/Linux with audio; if the host has no audio device, the test detects `Pa_Initialize()`/`Pa_GetDeviceCount()==0` and **skips** (prints `SKIP (no audio)` and returns 0 — mirror the RtMidi host test's skip pattern so headless CI stays green).
2. **Latency sane** — if output devices exist: `CHECK(port.output_latency_ms() >= 0.0)`.
3. **Open + close** (only if a device exists) — `CHECK(port.open(0, 2, 44100, 256) == 0)`; `CHECK(port.is_open())`; a render callback that writes `0.5`; wait ~120 ms; `CHECK(peak observed > 0.4f)` (the trampoline called `render`); `port.close()`; `CHECK(!port.is_open())`.
4. **Bad blocksize rejected** — `CHECK(port.open(0, 2, 44100, 100) == -1)` (not a multiple of 64).

- [ ] **Step 2: Run test to verify it fails**

`c++ -std=c++17 -Iextension/src -I<portaudio>/include extension/tests/audio_portaudio_tests.cpp <portaudio src/common *.c + os/unix *.c + hostapi/coreaudio *.c> extension/src/core/portaudio_port.cpp -o /tmp/audio_pa_tests -DPA_USE_COREAUDIO=1 -framework CoreAudio -framework CoreFoundation -framework CoreServices -framework AudioToolbox && /tmp/audio_pa_tests`
Expected: FAIL (no `PortAudioPort`).

- [ ] **Step 3: Implement `PortAudioPort`**

`portaudio_port.cpp`: `Pa_Initialize()` once (guard with `std::call_once`). `list_outputs()`/`list_inputs()`: iterate `Pa_GetDeviceCount()`, `Pa_GetDeviceInfo`, build `AudioDeviceInfo{index, name, maxInputChannels, maxOutputChannels}`; include only devices with `maxOutputChannels > 0` (outputs) / `maxInputChannels > 0` (inputs). `open(n_ins, n_out, samplerate, blocksize)`: reject `blocksize % 64 != 0` (`return -1`); `Pa_OpenStream(&stream, in, out, samplerate, blocksize, in?inputDev:null, outDev, pa_callback, this, &info)` selecting `Pa_GetDefaultOutputDevice()` (or the first device) for output and default input if `n_ins`; store `info.outputLatency`/`inputLatency` for the latency getters. `close()`: `Pa_CloseStream`. The C callback is exactly the trampoline above.

- [ ] **Step 4: Run test to verify it passes**

Re-run the Task 2 build line. Expected: PASS (or clean `SKIP` on a device-less host).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/portaudio_port.h extension/src/core/portaudio_port.cpp extension/tests/audio_portaudio_tests.cpp
git commit -m "native audio: PortAudioPort (CoreAudio/ALSA) enum + stream + latency (M5 T2)"
```

---

### Task 3: `NativeAudio` — orchestration, `RenderSlot`, RT `render_block`

Wires the port to the instances. Owns the `render_lock`. Its `render_block` is the actual audio-thread body: snapshot slots, `libpd_set_instance` + `libpd_process_float` per slot (under `render_lock`), then `mix_block` into the device output.

**Files:**
- Create: `extension/src/core/native_audio.h`
- Create: `extension/src/core/native_audio.cpp`
- Test: `extension/tests/native_audio_tests.cpp`

**Interfaces:**
- Consumes: `AudioPort` (Task 1), `RenderFn`, `mix_block` (Task 1), `libpd_*` (`libpd_set_instance`, `libpd_process_float`, `libpd_blocksize` — from `z_libpd.h`/`m_pd.h`).
- Produces (for Task 4/5):
  - `struct RenderSlot { t_pdinstance *pd = nullptr; int n_ins = 0; int n_out = 0; std::vector<float> in_buf; std::vector<float> out_buf; bool active = false; };`
  - `class NativeAudio { public:
      NativeAudio(); ~NativeAudio();
      bool open(AudioPort *p_port, int p_n_ins, int p_n_out, int p_blocksize, int p_samplerate); // takes ownership of p_port; wires render_block
      void close();
      bool is_open() const;
      AudioPort *port() const;
      int blocksize() const; int samplerate() const; int n_ins() const; int n_out() const;
      // Called on the main/control thread when an instance is ready / removed.
      int  register_instance(t_pdinstance *p_pd, int p_n_ins, int p_n_out);      // returns slot id (>= 0)
      void set_active(int p_slot, bool p_active);
      void unregister_instance(int p_slot);
      void with_render_lock(std::function<void()> p_fn);   // control-op RT guard (spec §6.2)
    private:
      void render_block(float *p_dev_in, float *p_dev_out, int p_frames);        // the RenderFn
      std::vector<RenderSlot> slots;  std::mutex slots_mu;
      std::mutex render_lock_;
      AudioPort *port = nullptr; int blocksize_ = 256; int samplerate_ = 44100; int n_ins_ = 0; int n_out_ = 2; bool open_ = false;
    };`
  - The `RenderSlot` in/out buffers are sized `blocksize * n_ins` / `blocksize * n_out` at `register_instance` time (fixed stream blocksize).

- [ ] **Step 1: Write the failing tests (host; NullPort driver)**

`extension/tests/native_audio_tests.cpp`:
1. **Render one instance** — build a real libpd instance on the test thread (`libpd_init(); auto *pd = libpd_new_instance(); libpd_set_instance(pd); libpd_init_audio(0, 2, 44100); ... "pd" "dsp"`), load a sine patch (a `[osc~ 440]`→`*~ 0.2`→`[dac~]` written to a temp file), `NativeAudio na; NullPort port; na.open(&port, 0, 2, 256, 44100);` `int slot = na.register_instance(pd, 0, 2); na.set_active(slot, true);` wait ~150 ms; `CHECK(port.frames_rendered() > 0)` and `CHECK(port.last_out_peak() > 0.1f)` (the sine at 0.2 gain → peak ~0.2). Then `na.set_active(slot, false); na.unregister_instance(slot); na.close();` `libpd_closefile(pd); libpd_free_instance(pd);`
2. **Input through libpd** — same as (1) but the patch is `[adc~ 1]`→`*~ 50`→`[dac~]`; `na.open(&port, 1, 2, 256, 44100)`; the `NullPort` render callback writes `0.001` into `dev_in`; wait ~150 ms; `CHECK(port.last_out_peak() > 0.04f)` (50 × 0.001). (This pins the `adc~` → `st_soundin` path end-to-end through the extension.)
3. **Two-instance mix clamps** — two sine patches at gain `1.0` (peak 1.0 each) into a 2-channel mix; wait ~150 ms; `CHECK(port.last_out_peak() <= 1.001f)` (clamped, not 2.0).
4. **Instance removed mid-render** — register 2 slots, activate both, wait ~100 ms, `unregister_instance(0)`, wait ~100 ms; assert no crash + `port.frames_rendered()` kept rising (the remaining slot still renders). (Pins the slots snapshot-under-lock invariant.)
5. **with_render_lock serializes** — `port` rendering; from the test thread, `na.with_render_lock([&]{ std::this_thread::sleep_for(50ms); })` × 10; assert no crash and `frames_rendered()` advanced (control op didn't wedge the audio thread).

- [ ] **Step 2: Run test to verify it fails**

Compile (same PortAudio + libpd link line as the spike; `build/cmake-macos/thirdparty/libpd/libs/libpd-multi.a` + PortAudio sources) — expected: FAIL (no `NativeAudio`).

- [ ] **Step 3: Implement `NativeAudio`**

`native_audio.cpp`:
- `open(port, n_ins, n_out, blocksize, samplerate)`: store config, `port->set_render_callback(&NativeAudio::render_block_trampoline, this)` — where the trampoline is a static that casts `this` and calls `render_block(dev_in, dev_out, frames)`. (`AudioPort::set_render_callback` takes a `RenderFn`; add an overload or wrap — see note: `RenderFn` is a raw C function, so use a static trampoline with `void*`.) `port->open(n_ins, n_out, samplerate, blocksize)`; `open_ = true` on success.
- `render_block(dev_in, dev_out, frames)`: `std::lock_guard<std::mutex> lk(render_lock_);` snapshot the slot list into a local `std::vector<RenderSlot*> active` under `slots_mu` (copy pointers to slots that are `active`); **for each slot:** `libpd_set_instance(slot->pd);` int `ticks = frames / 64;` `int n_out = slot->n_out;` call `libpd_process_float(ticks, slot->n_ins ? (float*)dev_in : nullptr, slot->out_buf.data())`; collect `slot->out_buf` pointers into a `const float* mix_in[]`; after the loop: `mix_block(dev_out, n_out_ /*device ch*/, mix_in, count, frames)`. (When `n_ins_ > 0`, `dev_in` is the shared device input; each slot reads its channels from it via `adc~`.)
- `register_instance(pd, n_ins, n_out)`: under `slots_mu`, push a `RenderSlot{pd, n_ins, n_out, in_buf(blocksize*n_ins), out_buf(blocksize*n_out), active=false}`; return its index.
- `with_render_lock(fn)`: `std::lock_guard lk(render_lock_); fn();`

- [ ] **Step 4: Run test to verify it passes**

Re-run the Task 3 build line. Expected: PASS (5/5).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/native_audio.h extension/src/core/native_audio.cpp extension/tests/native_audio_tests.cpp
git commit -m "native audio: NativeAudio render loop + slots + render_lock (M5 T3)"
```

---

### Task 4: Worker refactor — control thread (native) + `with_audio_lock` seam

Strip the a1 DSP/pacing loop when `native_audio` is true; keep it for Android. Add a `with_audio_lock` seam so control ops (INIT/LOAD/UNLOAD/STOP) serialize with the audio thread via `NativeAudio::with_render_lock`. Expose the worker's `pd_instance` + channel counts to the instance/server.

**Files:**
- Modify: `extension/src/libpd_worker.h` (add `Config.native_audio`, `Config.with_audio_lock`, accessors `pd_instance_ptr()`, `n_ins()`, `n_out()`).
- Modify: `extension/src/libpd_worker.cpp` (gate the DSP loop; wrap libpd control calls in `with_audio_lock`).

**Interfaces:**
- Consumes: `PdCommandQueue`, `libpd_*` (existing).
- Produces (for Task 5):
  - `Config.native_audio = false;` (bool; true → worker is control-only, no DSP loop).
  - `std::function<void(std::function<void()>)> Config.with_audio_lock;` (control-op RT guard; default = run the fn as-is, no lock).
  - `t_pdinstance *pd_instance_ptr() const;` (valid after INIT completes; the instance reads it to build its `RenderSlot`).
  - `int n_ins() const; int n_out() const;`

- [ ] **Step 1: Write the failing test**

Extend `extension/tests/worker_midi_tests.cpp` (or add `native_worker_tests.cpp`):
1. **Control-only in native mode** — `LibpdWorker::Config cfg; cfg.instance_id = 1; cfg.samplerate = 44100; cfg.n_out = 2; cfg.native_audio = true;` create worker, `start()`; push INIT (n_ins=0, n_out=2); wait for the result (existing `wait_for_command` pattern). Assert `worker.pd_instance_ptr() != nullptr`. Then push a LOAD of a sine patch; assert the load succeeds. Then **without any audio thread**, push a MESSAGE and assert no crash, and that `worker` did NOT push any audio (there is no sink in native mode — the sink pointer is null/ignored). `request_stop(); join();`. (Pins: native worker does init + load + message, and never renders on its own.)
2. **with_audio_lock is invoked** — set `cfg.with_audio_lock = [&](std::function<void()> f){ ++lock_calls; f(); };`; push INIT + LOAD; after completion `CHECK(lock_calls >= 2)` (INIT and LOAD each wrapped). (Pins the seam is called around the RT-sensitive libpd ops.)
3. **Android path still renders (regression)** — `cfg.native_audio = false;` + a `DrySink`; push INIT + dsp-on; wait ~100 ms; `CHECK(sink.blocks_pushed() > 0)`. (Guards the a1 loop still works when native is off.)

- [ ] **Step 2: Run test to verify it fails**

Re-run the `worker_midi_tests` build (CMake target exists). Expected: FAIL (`pd_instance_ptr`/`with_audio_lock` undefined).

- [ ] **Step 3: Implement the worker changes**

`libpd_worker.h`: add `bool native_audio = false;` and `std::function<void(std::function<void()>)> with_audio_lock;` to `Config`; add `t_pdinstance *pd_instance_ptr() const { return pd_instance; }`, `int n_ins() const { return config.n_ins; }`, `int n_out() const { return n_out; }` (or store `n_ins` too). A default `with_audio_lock` that just calls the fn is applied at the top of `run()` if `config.with_audio_lock` is null.

`libpd_worker.cpp` `run()`: replace the DSP branch with:
```
if (dsp_on.load() && pd_instance != nullptr && patch_handle != nullptr) {
    if (config.native_audio) {
        // Native: the audio thread renders; nothing to do here. Idle below.
    } else {
        ... existing a1 DSP + pacing block (unchanged) ...
    }
}
```
and in the idle path (no dsp, or native-with-dsp), keep the blocking `queue.pop(&command, 1000)` wait. Wrap the `libpd_*` control calls in `INIT`/`LOAD`/`UNLOAD` and the teardown `libpd_closefile`/`libpd_free_instance` with `config.with_audio_lock([&]{ ... })` (call the default if unset). Keep `libpd_start_message(1)`/`dsp` (they still enable the pd dsp engine; on native the render loop in `NativeAudio` does the actual `process_float`).

- [ ] **Step 4: Run test to verify it passes**

Re-run the worker test build. Expected: PASS (3/3 new + all existing).

- [ ] **Step 5: Commit**

```bash
git add extension/src/libpd_worker.h extension/src/libpd_worker.cpp extension/tests/worker_midi_tests.cpp
git commit -m "native audio: worker becomes control thread when native (a1 kept for Android) (M5 T4)"
```

---

### Task 5: Server `audio_*` API + instance registration + `init()` changes

The GDScript surface. The server owns a `NativeAudio`; instances register their `RenderSlot` with it on init and set active on dsp on/off. `audio_open` validates the block-size + samplerate contract. `init()` in native mode skips the Godot-mix-rate check (audio is native, not via `AudioServer`).

**Files:**
- Modify: `extension/src/libpd_server.h` / `libpd_server.cpp` (add `NativeAudio native_audio;`, the `audio_*` methods, `_bind_methods`, wire instance register/unregister to `native_audio`).
- Modify: `extension/src/libpd_instance.h` / `libpd_instance.cpp` (native branch in `init()`: set `worker.Config.native_audio` + `with_audio_lock`, register `RenderSlot`, register active on dsp on/off; `audio_slot` tracking; skip the mix-rate check in native mode).
- Test: extend `extension/tests/native_audio_tests.cpp` (or a new `server_audio_tests.cpp`) for the API-level contract.

**Interfaces:**
- Consumes: `NativeAudio` (Task 3), `PortAudioPort`/`NullPort` (Tasks 1/2), `LibpdWorker` accessors (Task 4).
- Produces (GDScript, via `_bind_methods`):
  - `bool LibpdServer::audio_open(int p_output_device, int p_input_device, int p_blocksize, int p_samplerate)` — `-1` for either device means default; `0` input device (index) is allowed; returns `true`/`false`. Validates `p_blocksize % 64 == 0` (else `push_error` + `false`) and that any already-`init`ed instance's `samplerate` equals `p_samplerate` (else `push_error` + `false`). Uses `PortAudioPort` on macOS/Linux (`NATIVE_AUDIO`).
  - `void LibpdServer::audio_close()`.
  - `Array LibpdServer::audio_list_output_devices()` — `[{index:int, name:String, max_channels:int}, …]`.
  - `Array LibpdServer::audio_list_input_devices()`.
  - `float LibpdServer::audio_output_latency_ms()` / `audio_input_latency_ms()`.
  - `bool LibpdServer::audio_available()` — true when the native backend is compiled in (`NATIVE_AUDIO`).
  - Signals (optional, M5.2): `audio_error(int code, String what)`.

- [ ] **Step 1: Write the failing tests (API contract, device-free)**

In a new `extension/tests/server_audio_tests.cpp` (or extend `native_audio_tests.cpp`), using `NullPort` as the backend so no device is needed — but the server uses `PortAudioPort` under `NATIVE_AUDIO`, so for a device-free host test, add a **test seam**: `LibpdServer` gets a `void _set_audio_port_for_test(std::unique_ptr<godot_libpd::AudioPort> port)` that `audio_open` uses instead of constructing a `PortAudioPort` (production path keeps `PortAudioPort`; the seam is test-only). Tests:
1. **blocksize not multiple of 64** — `server.audio_open(-1, -1, 100, 44100)` returns `false` (and `audio_error`/`push_error` fired).
2. **blocksize 64/128/256 accepted** — `audio_open(-1, -1, 128, 44100)` → `true`; `audio_output_latency_ms() >= 0`.
3. **samplerate mismatch fails** — `audio_open(-1,-1,256,44100)` → `true`; then a `LibpdInstance` `init(48000, 0, 2)` → returns `false` (mismatch) — but the instance is a Godot `Node`, hard to construct in a C++ test. So instead: register a fake slot with `samplerate 48000` via the worker and assert `audio_open(44100)` after it would reject. (If constructing a `LibpdInstance` in the test is impractical, pin this at the `NativeAudio::open` level: `open` rejects if a pre-registered slot's samplerate != stream samplerate — add that check + test it directly on `NativeAudio`.)
4. **audio_available** — `CHECK(server.audio_available() == true)` under `NATIVE_AUDIO`.

- [ ] **Step 2: Run test to verify it fails**

Re-run the server test build. Expected: FAIL (no `audio_*` methods).

- [ ] **Step 3: Implement the server + instance changes**

`libpd_server.h/.cpp`: add `#include "core/native_audio.h"` + `std::unique_ptr<godot_libpd::AudioPort> audio_port_override_;` + `godot_libpd::NativeAudio native_audio;`. Implement `audio_open`: validate blocksize, build the port (`_set_audio_port_for_test` override or `std::make_unique<PortAudioPort>` under `NATIVE_AUDIO`), `native_audio.open(port, in_dev?n_in:0, out_dev?n_out:2, blocksize, samplerate)`; validate pre-registered instances' samplerates; on failure `push_error` + `false`. `audio_close`: `native_audio.close()`. The `list_*`/`latency`/`audio_available` delegate to `native_audio.port()` (or a fresh `PortAudioPort` for enumeration when closed). `_bind_methods`: register `audio_open`, `audio_close`, `audio_list_output_devices`, `audio_list_input_devices`, `audio_output_latency_ms`, `audio_input_latency_ms`, `audio_available` (+ `audio_error` signal if added).

`libpd_instance.cpp`: in `init()`, branch on `#ifdef NATIVE_AUDIO` (or a server-provided `audio_available()`): native → set `worker.Config.native_audio = true;`, `worker.Config.with_audio_lock = [&](auto f){ server->native_audio.with_render_lock(f); };` (capture the server), remove the `AudioServer` mix-rate check (record `samplerate_value = p_samplerate`), and after the INIT command returns success, `audio_slot = server->native_audio.register_instance(worker.pd_instance_ptr(), p_n_ins, p_n_out);`. In `start_dsp()`/`stop_dsp()`, after pushing the command, call `server->native_audio.set_active(audio_slot, true/false);` (guarded by `audio_slot >= 0`). In `_exit_tree`, `native_audio.set_active(audio_slot, false)` + `unregister_instance(audio_slot)`. Keep the a1 path (generator/sink/player) under `#ifndef NATIVE_AUDIO` (Android). Store `int audio_slot = -1;`.

- [ ] **Step 4: Run test to verify it passes**

Re-run the server test build + the full host `ctest`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/libpd_server.h extension/src/libpd_server.cpp extension/src/libpd_instance.h extension/src/libpd_instance.cpp extension/tests/server_audio_tests.cpp
git commit -m "native audio: server audio_* API + instance RenderSlot registration (M5 T5)"
```

---

### Task 6: CMake — PortAudio vendor + `NATIVE_AUDIO` + test wiring

Build the vendored PortAudio, define `NATIVE_AUDIO` for macOS/Linux, link it into the extension, and register the new test targets.

**Files:**
- Modify: `extension/CMakeLists.txt`.

**Interfaces:**
- Consumes: the vendored PortAudio tree (`thirdparty/libpd/pure-data/portaudio/portaudio/{include, src/common, src/os/unix, src/hostapi/coreaudio, src/hostapi/alsa}`).
- Produces: a `portaudio` static target + `NATIVE_AUDIO` define + the new `add_test` entries.

- [ ] **Step 1: Add the PortAudio static library**

In `extension/CMakeLists.txt`, after the RtMidi block, add (guarded by `NATIVE_AUDIO = NOT ANDROID`):
```
set(NATIVE_AUDIO ON)
if(ANDROID)
    set(NATIVE_AUDIO OFF)
endif()
if(NATIVE_AUDIO)
    set(PA_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_BINARY_PROJECT OFF CACHE BOOL "" FORCE)
    set(PA_INSTALL_REGISTRAR OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/libpd/pure-data/portaudio/portaudio EXCLUDE_FROM_ALL)
    target_link_libraries(godot_libpd PRIVATE portaudio)
    target_compile_definitions(godot_libpd PRIVATE NATIVE_AUDIO)
    if(APPLE)
        target_compile_definitions(godot_libpd PRIVATE PA_USE_COREAUDIO=1)
        target_link_libraries(godot_libpd PRIVATE
            "-framework CoreAudio" "-framework CoreFoundation"
            "-framework CoreServices" "-framework AudioToolbox")
    else()
        target_compile_definitions(godot_libpd PRIVATE PA_USE_ALSA=1)
        target_link_libraries(godot_libpd PRIVATE asound)
    endif()
endif()
```
(Add the `portaudio` `add_subdirectory` result to the existing `target_link_libraries(godot_libpd ...)` chain — the block above does it inline.)

- [ ] **Step 2: Wire the new test targets**

Under the existing `if(BUILD_LIBPD_TESTS AND NOT ANDROID)` block, add:
- `audio_port_tests` (Task 1): `add_executable(audio_port_tests tests/audio_port_tests.cpp)`, include `src`, link `pthread`; `add_test(NAME audio_port_tests ...)`.
- `audio_portaudio_tests` (Task 2): `add_executable(...) tests/audio_portaudio_tests.cpp src/core/portaudio_port.cpp`, include `src` + the PortAudio `include` dir, link `portaudio pthread` + the platform frameworks/libasound + `PA_USE_*`; `add_test(...)`.
- `native_audio_tests` (Task 3): `add_executable(...) tests/native_audio_tests.cpp src/core/native_audio.cpp src/core/portaudio_port.cpp`, link `libpd_static portaudio pthread` + frameworks + `NATIVE_AUDIO`/`PA_USE_*`; `add_test(...)`.
- `server_audio_tests` (Task 5): `add_executable(...) tests/server_audio_tests.cpp src/libpd_server.cpp src/libpd_instance.cpp src/libpd_worker.cpp src/core/native_audio.cpp src/core/portaudio_port.cpp src/core/pd_command_queue.cpp src/core/pd_event_ring.cpp src/core/pd_debug.cpp src/midi_router.cpp src/midi_backend_factory.cpp`, link `godot-cpp libpd_static portaudio pthread` + frameworks + `NATIVE_AUDIO`; `add_test(...)`.

- [ ] **Step 3: Build the full extension + run all host tests**

Run: `./extension/build.sh` (or the platform build) then `cd extension/build/cmake-<platform> && ctest --output-on-failure`.
Expected: **all existing tests pass + the new audio tests pass** (or clean `SKIP` where no device).

- [ ] **Step 4: Commit**

```bash
git add extension/CMakeLists.txt
git commit -m "native audio: CMake PortAudio vendor + NATIVE_AUDIO + test wiring (M5 T6)"
```

---

### Task 7: Test app native mode + macOS on-device verification

Point the test app at the native backend and prove, on a real Mac, that a `[adc~]`-fed patch renders to speakers (input) and a synth renders (output). This is the acceptance leg (spec §10, M5).

**Files:**
- Modify: `test_project/scripts/test_audio.gd` (and/or a new `test_native_audio.gd` + `test_native_audio.tscn`).

**Interfaces:**
- Consumes: the `Libpd` autoload (`audio_open`, `audio_list_output_devices`, `audio_list_input_devices`, `audio_output_latency_ms`, `audio_available`).

- [ ] **Step 1: Add a native-audio test scene**

Create `test_project/scenes/test_native_audio.tscn` + `test_project/scripts/test_native_audio.gd` (GDScript 4.6 — **`#` comments only, no `//`**): on `_ready`, print `Libpd.audio_available()`; if false, `push_error` + return; list output + input devices; `Libpd.audio_open(-1, -1, 256, 44100)`; create two `LibpdInstance` children, `init(44100, 0, 2)`, load a sine patch + a loopback (`[adc~ 1]`→`*~ 50`→`[dac~]`) patch, `start_dsp()` on both; after 2 s, print `Libpd.audio_output_latency_ms()`.

- [ ] **Step 2: Build the macOS app + run**

Rebuild the extension (Task 6), rebuild the macOS arm64 export (the existing custom template), export the test project, run the app, and confirm via console: devices listed, `audio_open` true, both instances dsp-on, latency printed.
Expected: the app prints the device list + latency; **sine is audible** (output) and **mic-driven sound is audible** (input, through `[adc~]`).

- [ ] **Step 3: On-device verification (HARD VERIFICATION RULE)**

Run the app on the Mac; capture the console log (paste it). Confirm: `audio_available()==true`, `audio_open()==true`, `audio_list_output_devices()` non-empty, `audio_output_latency_ms() < 20.0` (near the spike's ~28 ms floor at block=256, or better). Paste the real command + output.

- [ ] **Step 4: Commit**

```bash
git add test_project/scenes/test_native_audio.tscn test_project/scripts/test_native_audio.gd
git commit -m "native audio: test app native mode + macOS verification (M5 T7)"
```

---

## Self-Review (writing-plans checklist)

**1. Spec coverage:** §2 (facts) → Global Constraints. §4 Approach A + `pd_this` (4.5) → Task 3 (`render_block` sets instance) + Global Constraints. §4.4 per-thread → Task 3/4. §4.4 worker refactor → Task 4. §5 backends (CoreAudio/ALSA; M6 AAudio out of scope) → Task 2 + Task 6. §6.1 block-size/samplerate contract → Task 1 (`mix_block`), Task 2 (reject 100), Task 5 (`audio_open` validation). §6.2 openfile RT-safety → Task 4 (`with_audio_lock`) + Task 3 (`render_lock`). §6.3 multi-instance mix → Task 1 (`mix_block`) + Task 3 test 3. §6.4 no-device fallback → Task 1 (`NullPort`). §7 API → Task 5. §8 platform matrix → Task 7 (M5 macOS; Linux/Knulli is the same `ALSA` path in Task 2/6, verified in a follow-up). §10 success criteria → Task 7. **Gap flagged:** Linux/Knulli on-device (fbdev, `ALSA`) is not a separate task here — it reuses Task 2's `ALSA` + Task 6's `PA_USE_ALSA`; add a follow-up verification step when a Knulli is in hand (the host `ALSA` unit test already gates the code path).

**2. Step scan:** every step pins one action with a checkable result. `mix_block` body given (Task 1) because the clamp behavior is the whole point; `PortAudioPort`/`NativeAudio` bodies left to the implementer (signatures + the spike-confirmed contract determine them). No "handle edge cases" lines.

**3. Type consistency:** `AudioPort`/`RenderFn`/`AudioDeviceInfo`/`mix_block` (Task 1) → used identically in Tasks 2/3/5/6. `NativeAudio::open/register_instance/set_active/unregister_instance/with_render_lock` (Task 3) → called in Task 4/5. `Config.native_audio`/`with_audio_lock` + `pd_instance_ptr()` (Task 4) → consumed in Task 5. `LibpdServer::audio_open` signature consistent in Task 5/7.

**4. Review Focus:** all five lines map to a task test (pd_this → T3 test1; blocksize → T2 test4 + T5 test1; samplerate → T5 test3; mix clamp → T1 test3; openfile stall → T4 test + T3 test5; remove-mid-render → T3 test4). Six lines, all covered.

**5. Proportion:** the plan is ~1.5× the spec's §M5 length — it adds the file-level decomposition + test assertions, not a transcript of the code. Bodies appear only where the spec/signature don't determine them (`mix_block` clamp, `render_block` loop, the CMake PortAudio block).

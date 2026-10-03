# v2 M5 — Native Audio Backend (macOS + Linux) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Route libpd audio through native PortAudio on macOS (CoreAudio) + Linux (ALSA/Knulli): **8 synth `LibpdInstance` workers** (a1, one pinned audio thread each) feed their stereo pairs into **`MixInputRing`s**; a dedicated **mix-down `LibpdInstance`** (16-in/2-out patch, overall EQ+FX) is **rendered in the PortAudio real-time callback**; the mixed stereo goes to the device. Retire the a1 `AudioStreamGenerator` on desktop (Android keeps a1 until M6).

**Architecture:** *Revised 2026-10-03.* The original "Approach A" (one shared RT thread rendering all instances) is **infeasible for multi-instance**: repro-confirmed that switching `libpd_set_instance` between live instances mid-dsp crashes pd's global dsp/scheduler state (`EXC_BAD_ACCESS`) — even 2 instances merely existing. One **dedicated audio thread per instance** (the a1 model, `set_instance` once per thread) supports **9+ concurrent instances** (repro: 8 stereo workers + 16-in/2-out mix-down on a separate render thread, no crash). So M5 keeps the a1 per-instance worker threads for the synths and moves the mix into a mix-down instance rendered on the PortAudio callback.

**Tech Stack:** godot-cpp GDExtension, C++17, **vendored PortAudio 19.7.0** (built **in place** from `extension/thirdparty/libpd/pure-data/portaudio/portaudio/`), libpd-static (`libpd-multi.a`, PD_MULTI ON), CMake/ctest.

**Spec:** `docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md` (status: revised 2026-10-03).

## Done (do not re-do)

- **T1 — `AudioPort` + `NullPort` + `mix_block`** (commit `f385d040`): `extension/src/core/audio_port.h` (`AudioPort` base with `open/close/list_*/latency_*` + `set_render_callback(std::function<void(float*,float*,int)>)`; pure `mix_block` sum+clamp), `extension/src/core/null_port.h` (`NullPort`, device-free fake audio thread, `frames_rendered()`/`last_out_peak()`), `extension/tests/audio_port_tests.cpp`.
- **T2 — `PortAudioPort` (CoreAudio/ALSA)** (commit `6810a865`): `extension/src/core/portaudio_port.{h,cpp}` (device enum, full-duplex stream, static C callback trampoline → the render `std::function`, latency from `PaStreamInfo`, rejects `blocksize%64!=0`, `Pa_StartStream` after open). `extension/tests/audio_portaudio_tests.cpp`.

## Global Constraints

- **THE threading invariant:** each libpd instance's `libpd_process_float` runs on **exactly one thread** that called `libpd_set_instance(it)` **exactly once**. **No thread ever switches `pd_this` between instances** — that crashes pd (the reason Approach A was abandoned). Synth workers = one thread each; the mix-down renders on the PortAudio callback thread (its one thread, `set_instance(mix)` once). (Spec §4.)
- **9 concurrent instances is a hard requirement** — the plan's regression test must exercise 8 synth workers + 1 mix-down live.
- **Block-size contract:** `libpd_blocksize()==64`; device `blocksize` MUST be a multiple of 64; `ticks = blocksize/64`. Default 256. (Spec §6.)
- **No resampling:** the mix-down's `samplerate` and every synth's `samplerate` MUST equal the stream `samplerate`; mismatch fails fast.
- **Channel layout:** each synth worker outputs a **stereo pair** (2 ch) into its `MixInputRing`; the mix-down has **16 inputs** (8 stereo pairs) + **2 outputs**. Ring `i` → mix-down input channels `2i, 2i+1`. (Spec §4.3.)
- **mix_render_lock:** the mix-down's **control ops** (its worker thread: INIT/`init_audio`, LOAD/`openfile`, UNLOAD/`closefile`, teardown) are serialized with the **callback render** by `NativeAudio`'s `mix_render_lock` — a brief audio stall while the mix patch loads (Spec §7.2).
- **No-device fallback:** `NullPort` preserves headless/host tests + Knulli CI.
- **Mix-down callback discipline:** the PortAudio callback does only: a **fixed-buffer gather** (no allocation) of the 8 rings → 16 ch, one `libpd_set_instance(mix)` (once), one `libpd_process_float`, one copy to the device. No I/O, no Godot/MIDI, no blocking.
- **Platform split:** `NATIVE_AUDIO` ON for macOS + Linux, OFF for Android. Android synth/mixer keep the a1 `AudioStreamGenerator` path.
- **Input object is `adc~`** (1-indexed); no `audioin~` class in this pd build. The mix-down patch uses `[adc~ 1..16]`.
- **PortAudio build (spike-confirmed):** CoreAudio needs `-framework AudioToolbox` + `CoreFoundation` + `CoreServices` + `-DPA_USE_COREAUDIO=1`; ALSA needs `-DPA_USE_ALSA=1` + `asound`. PortAudio root `PA = extension/thirdparty/libpd/pure-data/portaudio/portaudio`; libpd include dirs `extension/thirdparty/libpd/libpd_wrapper` + `.../pure-data/src`; libpd lib `extension/build/cmake-<plat>/thirdparty/libpd/libs/libpd-multi.a`.
- **Subagent dispatches MUST set `timeoutMs: 7200000`** (2 h) — the default 30 min is too short for the multi-instance/native tasks. HARD VERIFICATION RULE for on-device claims.
- libpd init/render sequence (verbatim from the working `spike/native_audio/spike.c` + `spike/mix_repro.c`): INIT (on the instance's thread): `libpd_init()` (once, process-global) → `libpd_new_instance()` → `libpd_set_instance(it)` → `libpd_init_audio(n_ins,n_out,samplerate)` → `libpd_start_message(1); libpd_add_float(1.0f); libpd_finish_message("pd","dsp")` → `libpd_openfile(file, dir)`. Render (on the pinned thread): `libpd_process_float(ticks, in, out)`.

## Review Focus

- **No instance-switching** — confirm no thread calls `libpd_set_instance` for more than one instance across its lifetime; the callback calls it **exactly once** (for the mix-down). A second/switching call is a Critical finding (it crashes pd). (T4.)
- **9 concurrent instances** — the regression test must actually spawn 8 synth workers + the mix-down and run them live ~0.5 s without a crash. A test that only covers 1–2 instances is an Important finding. (T4/T7.)
- **Ring latest-vs-stale + underrun** — `MixInputRing.gather_latest` returns the newest complete block; an empty ring contributes silence (0), not garbage. Producer/consumer must not race (no torn block). (T3.)
- **mix_render_lock correctness** — a mix-patch `load` while the callback is rendering must not corrupt (the lock serializes); no deadlock (single lock, no nesting: the callback holds it only around `process_float`; the worker holds it only around its control ops). (T4/T5.)
- **set_mixer before render** — the callback must be null-safe before `set_mixer` (contribute silence), and must not call `process_float` on a null `mix_pd`. (T4.)
- **Samplerate/blocksize contract** — `audio_open(100)` (not a multiple of 64) fails; a synth `init(48000)` with a 44100 stream fails fast. (T6.)

---

### Task 3: `MixInputRing` — per-synth-worker ring

The handoff buffer between a synth worker (producer) and the mix-down callback (consumer). Host-testable, no libpd, no device.

**Files:**
- Create: `extension/src/core/mix_input_ring.h` (header-only is fine) 
- Test: `extension/tests/mix_input_ring_tests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (for Tasks 4/5):
  - `namespace godot_libpd { class MixInputRing { public:
      explicit MixInputRing(int p_channels = 2, int p_blocksize = 256, int p_num_blocks = 8);
      void push(const float *p_block, int p_frames);            // producer: worker
      int  gather_latest(float *p_dst, int p_frames) const;     // consumer: callback; returns frames copied (0 if empty)
      int channels() const; int blocksize() const; int num_blocks() const;
    private:
      // circular buffer of p_num_blocks slots, each blocksize*channels floats;
      // head index protected by a single mutex (low-frequency SPSC) OR lock-free;
      // "latest wins": gather copies the newest written slot.
    }; }`
  - `gather_latest` writes `p_frames * channels` floats into `p_dst` (the newest complete block); if the ring has no block yet it writes **0.0** to every `p_dst` sample and returns 0. The `frames` must equal the ring's `blocksize`.

- [ ] **Step 1: Write the failing tests**

`extension/tests/mix_input_ring_tests.cpp` (CHECK/main style, mirror `audio_port_tests.cpp`):
1. **Push then gather returns latest** — `MixInputRing r(2,256,8);` push block A (all `0.25f`), push block B (all `0.5f`); `gather_latest(dst,256)`; `CHECK(dst` all ≈ `0.5f)` and returns 256.
2. **Latest-wins after many pushes** — push 20 distinct blocks; `gather_latest` returns the 20th block's value.
3. **Empty ring → silence** — fresh ring, `gather_latest(dst,256)` returns 0 and `dst` is all 0.0.
4. **Producer/consumer no race** — a producer thread pushes 500 blocks (incrementing a marker per block); a consumer thread calls `gather_latest` 500 times; assert no crash and that every gathered block is either all-silence or a consistent single marker value (no torn/mixed block).
5. **Overflow wraps (no growth)** — push `num_blocks*10` blocks; gather returns the most recent; the ring never grows (fixed size).

- [ ] **Step 2: Run test to verify it fails**

`c++ -std=c++17 -Iextension/src extension/tests/mix_input_ring_tests.cpp -o /tmp/mix_input_ring_tests -pthread && /tmp/mix_input_ring_tests`
Expected: FAIL (no `MixInputRing`).

- [ ] **Step 3: Implement `MixInputRing`**

Header-only. A `std::vector<float>` circular buffer sized `num_blocks * blocksize * channels`; a `std::mutex` (or atomic head) guarding the newest-slot index; `push` writes the block to the next slot and advances; `gather_latest` copies the newest slot to `p_dst` (0.0-filled when empty). Keep it allocation-free after construction (the buffer is fixed).

- [ ] **Step 4: Run test to verify it passes**

Re-run the Step 2 command. Expected: PASS (5/5).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/mix_input_ring.h extension/tests/mix_input_ring_tests.cpp
git commit -m "native audio: MixInputRing per-synth-worker ring (M5 T3)"
```

---

### Task 4: `NativeAudio` — mix-down render in the PortAudio callback

Replace the (Approach-A) `NativeAudio` written during the blocked run with the mix-down-in-callback object. It owns the PortAudio stream + the callback that gathers the synth rings and renders the mix-down instance.

**Files:**
- Modify: `extension/src/core/native_audio.h` / `native_audio.cpp` (rewrite — the existing file is Approach-A; keep the `AudioPort*` ownership + `mix_block`/`NullPort` fallback, drop the per-slot `render_block` multi-instance loop).
- Test: `extension/tests/native_audio_mixdown_tests.cpp`

**Interfaces:**
- Consumes: `AudioPort`/`NullPort`/`mix_block` (T1), `MixInputRing` (T3), `PortAudioPort` (T2, for the real device; tests use `NullPort`), `libpd_*`.
- Produces (for Tasks 5/6):
  - `class NativeAudio { public:
      NativeAudio(); ~NativeAudio();
      bool open(AudioPort *p_port, int p_mix_n_in, int p_mix_n_out, int p_blocksize, int p_samplerate); // owns p_port
      void close();
      bool is_open() const; AudioPort *port() const;
      int blocksize() const; int samplerate() const;
      void set_mixer(t_pdinstance *p_mix_pd, int p_mix_n_in, int p_mix_n_out); // called on main thread before the mixer's start_dsp
      void register_worker_ring(MixInputRing *p_ring);   // order defines the 16ch layout (ring i -> ch 2i,2i+1)
      void unregister_worker_ring(MixInputRing *p_ring);
      void with_mixer_lock(std::function<void()> p_fn);  // the mixer's control ops take this (spec §7.2)
    private:
      void render_block(float *p_dev_in, float *p_dev_out, int p_frames);  // the callback body
      std::vector<MixInputRing*> rings_; std::mutex rings_mu_;
      t_pdinstance *mix_pd_ = nullptr; int mix_n_in_=16, mix_n_out_=2; std::atomic<bool> mix_set_{false};
      std::mutex mix_render_lock_; std::vector<float> mix_in_; std::vector<float> mix_out_;
      AudioPort *port_=nullptr; int blocksize_=256, samplerate_=44100; bool open_=false;
    };`

- [ ] **Step 1: Write the failing tests** (host; NullPort + real libpd mix instance)

`extension/tests/native_audio_mixdown_tests.cpp`:
1. **Mix-down renders** — build a real libpd mix instance on the test thread: `libpd_init(); auto* mix=libpd_new_instance(); libpd_set_instance(mix); libpd_init_audio(16,2,44100); ...dsp...; libpd_openfile(<16-in/2-out patch>, dir)` where the patch is `[adc~ 1] *~ 1.0 [dac~]` (reads channel 1 → out). `NullPort port; NativeAudio na; na.open(&port,16,2,256,44100);` create a `MixInputRing ring0(2,256,8); na.register_worker_ring(&ring0);` a producer thread pushes `0.5f` blocks into `ring0`; `na.set_mixer(mix,16,2);` wait ~250 ms; `CHECK(port.frames_rendered()>0)` and `CHECK(port.last_out_peak()>0.4f)`. Tear down: `na.close();` then (audio thread joined) `libpd_closefile; libpd_free_instance(mix);`
2. **16-ch gather** — feed 8 rings; a mix patch `[adc~ 1]` and a second check `[adc~ 16]`. Feed only ring 0 with 0.5 → `[adc~ 1]` output ≈0.5, `[adc~ 16]` output ≈0. Then feed only ring 7 with 0.5 → `[adc~ 16]` ≈0.5, `[adc~ 1]` ≈0. (Proves the ring→channel mapping.)
3. **set_mixer not called → silence** — open + register a ring fed with 0.5, but do NOT `set_mixer`; wait ~200 ms; `CHECK(port.last_out_peak()==0.0f)` and no crash (callback null-safe).
4. **with_mixer_lock serializes** — while the test thread holds `na.with_mixer_lock([&]{ sleep 50ms; })` × 5, `port.frames_rendered()` is frozen; after release it resumes. No crash.
5. **9-concurrent regression** — 8 real synth instances, each on its **own** pthread (a1: `new_instance; set_instance once; init_audio(0,2,44100); dsp; openfile(<sine>); loop process_float(1,nullptr,buf)`), each pushing its 2-ch `buf` into its own `MixInputRing`. 1 mix instance rendered by `NativeAudio` on a `NullPort` (a `[adc~ 1] *~ 0.25 [dac~]`-style patch). Register all 8 rings, `set_mixer(mix)`, run ~600 ms. `CHECK(no crash)` and `port.frames_rendered()>0` and `last_out_peak()>0`. (This ports `spike/mix_repro.c` into the suite — the key multi-instance guard.)

- [ ] **Step 2: Run test to verify it fails**

Compile (same recipe as the spike/mix_repro: c++ for the `.cpp`s + the libpd `-I` dirs + `libpd-multi.a` + PortAudio in the test via `NullPort` which needs no PortAudio link; the mix-down test only needs libpd + `null_port` + `native_audio`):
`c++ -O2 -DPA_USE_COREAUDIO=1 -I <PA>/include -I extension/thirdparty/libpd/libpd_wrapper -I extension/thirdparty/libpd/pure-data/src -I extension/src extension/tests/native_audio_mixdown_tests.cpp extension/src/core/native_audio.cpp extension/build/cmake-macos/thirdparty/libpd/libs/libpd-multi.a -lpthread -framework CoreAudio -framework CoreFoundation -framework CoreServices -framework AudioToolbox -o /tmp/native_audio_mixdown_tests && /tmp/native_audio_mixdown_tests`
Expected: FAIL (new `NativeAudio` API / behavior).

- [ ] **Step 3: Implement `NativeAudio` (mix-down-in-callback)**

`native_audio.cpp`:
- `open(port, mix_n_in, mix_n_out, blocksize, samplerate)`: validate args (blocksize%64, samplerate>0, mix_n_out>0); size `mix_in_` (`blocksize*mix_n_in`) + `mix_out_` (`blocksize*mix_n_out`); `port->set_render_callback([this](float*in,float*out,int f){ this->render_block(in,out,f); })`; `port->open(mix_n_in, mix_n_out, samplerate, blocksize)`; `open_=true`. (The device `n_ins` is 0 — the mix-down's input is the gathered rings, not the device; pass 0 to `port->open`'s input count. See note below.)
- `render_block(dev_in, dev_out, frames)`:
  ```
  // gather the 8 rings -> mix_in_ (16ch). Fixed buffers, no alloc.
  for i, ring in rings_ (snapshot under rings_mu_):
      int got = ring->gather_latest(&mix_in_[i*frames*2], frames);  // writes 0 if empty
  // one-shot set_instance on THIS thread (the callback thread)
  if (mix_pd_ != nullptr && !mix_set_.exchange(true)) libpd_set_instance(mix_pd_);
  if (mix_pd_ == nullptr) { /* silence */ zero dev_out; return; }
  std::lock_guard lk(mix_render_lock_);   // serialize with the mixer's control ops
  int ticks = frames / 64;
  libpd_process_float(ticks, mix_in_.data(), mix_out_.data());
  copy mix_out_ -> dev_out (frames * mix_n_out_);
  ```
- `set_mixer(mix_pd, n_in, n_out)`: store under a small lock; `mix_set_` stays false until the first callback (the callback sets the instance on its own thread — the control thread must NOT call `set_instance(mix)`; only the callback does).
- `with_mixer_lock(fn)`: `std::lock_guard lk(mix_render_lock_); fn();`.
- Note: the PortAudio stream's **device input** is unused (the mix-down's input is the rings). Open the PortAudio stream with **0 device inputs** (`port->open(0, mix_n_out, samplerate, blocksize)`); `mix_n_in` is only used to size `mix_in_` and is the mix instance's `libpd_init_audio` channel count, not the device's.

- [ ] **Step 4: Run test to verify it passes**

Re-run the Step 2 command. Expected: PASS (5/5).

- [ ] **Step 5: Commit**

```bash
git add extension/src/core/native_audio.h extension/src/core/native_audio.cpp extension/tests/native_audio_mixdown_tests.cpp
git commit -m "native audio: NativeAudio mix-down render in PortAudio callback (M5 T4)"
```

---

### Task 5: Worker refactor — synth ring sink + mixer control-only

The synth worker keeps the a1 render loop but pushes to its `MixInputRing`; the mixer worker is control-only (the callback renders it); its control ops take `with_mixer_lock`. Android is untouched.

**Files:**
- Modify: `extension/src/libpd_worker.h` / `libpd_worker.cpp`

**Interfaces:**
- Consumes: `MixInputRing` (T3), `libpd_*` (existing), `PdCommandQueue`.
- Produces (for Task 6):
  - `enum class WorkerRole { SYNTH, MIXER, ANDROID };`
  - `Config` gains: `WorkerRole role = ANDROID;` (default = existing a1/generator behavior), `MixInputRing *worker_ring = nullptr;` (SYNTH), `std::function<void(std::function<void()>)> with_mixer_lock;` (MIXER; default = run fn as-is).
  - `t_pdinstance *pd_instance_ptr() const;` (valid after INIT).
- **SYNTH** worker: keep the existing a1 DSP+pacing loop, but replace `config.sink->push_block(...)` with `config.worker_ring->push(out_buffer.data(), blocksize)` (2-ch). Pacing unchanged (paced to samplerate).
- **MIXER** worker: **no render loop** (the PortAudio callback renders the mix). Wrap its `libpd_*` control ops — INIT (incl. `libpd_init_audio`), LOAD (`openfile`), UNLOAD (`closefile`), and teardown (`closefile`+`free_instance`) — in `config.with_mixer_lock`.
- **ANDROID** worker: existing a1 DSP+pacing + `sink->push_block` (unchanged).

- [ ] **Step 1: Write the failing tests**

Extend `extension/tests/worker_midi_tests.cpp` (or a new `native_worker_tests.cpp`):
1. **SYNTH worker pushes to ring** — `Config{role=SYNTH, worker_ring=&ring, samplerate=44100, n_out=2};` start worker; push INIT (0 in / 2 out); push a LOAD of a sine patch; push dsp-on. Wait ~150 ms; `CHECK(ring has blocks)` (a `blocks_pushed()` counter on the ring, or gather returns non-silence). `request_stop(); join();`
2. **MIXER worker is control-only** — `Config{role=MIXER, with_mixer_lock=[&](auto f){ ++calls; f(); }};` push INIT + LOAD; after completion `CHECK(calls >= 2)` (INIT and LOAD each wrapped in `with_mixer_lock`); and the worker pushed **no** audio (no ring, no sink). (Pins the mixer does not render.)
3. **ANDROID worker still renders (regression)** — `Config{role=ANDROID, sink=&drySink};` INIT + dsp-on; wait ~100 ms; `CHECK(drySink.blocks_pushed()>0)`.

- [ ] **Step 2: Run test to verify it fails**

Re-run the worker test build. Expected: FAIL (`role`/`worker_ring`/`with_mixer_lock` undefined).

- [ ] **Step 3: Implement the worker changes**

In `run()`, branch on `config.role`:
```
bool render = dsp_on.load() && pd_instance && patch_handle;
if (render && config.role == WorkerRole::SYNTH) {
    ... existing a1 DSP+pacing block, but push to config.worker_ring instead of config.sink ...
} else if (render && config.role == WorkerRole::ANDROID) {
    ... existing a1 DSP+pacing block, push to config.sink (unchanged) ...
} else {
    // SYNTH-idle / MIXER (control-only) / not-ready: block on queue.pop(&command, 1000)
}
```
In `execute_command`, wrap the MIXER's RT-sensitive ops in `config.with_mixer_lock([&]{ ... })` (INIT body, LOAD body, UNLOAD body) and the teardown in `run()`'s close-file/free-instance. Apply a default no-op `with_mixer_lock` if unset.

- [ ] **Step 4: Run test to verify it passes**

Re-run the worker test build. Expected: PASS (3/3 new + existing).

- [ ] **Step 5: Commit**

```bash
git add extension/src/libpd_worker.h extension/src/libpd_worker.cpp extension/tests/native_worker_tests.cpp
git commit -m "native audio: worker roles — synth ring sink + mixer control-only (M5 T5)"
```

---

### Task 6: Server `audio_*` API + `set_mixer` + instance roles

The GDScript surface. The server owns `NativeAudio`; instances are assigned SYNTH vs MIXER roles.

**Files:**
- Modify: `extension/src/libpd_server.h` / `libpd_server.cpp`
- Modify: `extension/src/libpd_instance.h` / `libpd_instance.cpp`
- Test: `extension/tests/server_audio_tests.cpp`

**Interfaces:**
- Consumes: `NativeAudio` (T4), `PortAudioPort`/`NullPort` (T1/T2), `MixInputRing` (T3), `LibpdWorker` (T5).
- Produces (GDScript, via `_bind_methods`):
  - `bool LibpdServer::audio_open(int p_blocksize, int p_samplerate, int p_mix_in = 16, int p_mix_out = 2)` — validates `blocksize%64==0` (else `push_error`+false); opens a `PortAudioPort` stream (0 device inputs, `p_mix_out` device outputs) via `native_audio.open(...)`. Under `#ifndef NATIVE_AUDIO` (Android) → returns false.
  - `void LibpdServer::audio_close()`.
  - `bool LibpdServer::set_mixer(LibpdInstance *p_mix)` — designate the mix-down; after its worker INIT, `native_audio.set_mixer(mix->pd_instance_ptr(), 16, 2)`. Fails if audio not open.
  - `Array audio_list_output_devices()` / `audio_list_input_devices()` / `float audio_output_latency_ms()` / `bool audio_available()`.
- `LibpdInstance`:
  - a new role (default SYNTH; `set_mixer` marks the mixer).
  - **SYNTH** `init()`: creates its `MixInputRing(2, stream_blocksize, 8)`; sets `worker.Config.role=SYNTH; Config.worker_ring=&ring;`; after INIT success, `server->native_audio.register_worker_ring(&ring)`.
  - **MIXER** `init()`: sets `worker.Config.role=MIXER; Config.with_mixer_lock=[&](auto f){ server->native_audio.with_mixer_lock(f); };`; its `n_ins` MUST be 16, `n_out` 2 (the mix layout); after INIT, the server's `set_mixer` wires `native_audio.set_mixer(pd,16,2)`.
  - Native `init()` skips the Godot `AudioServer` mix-rate check (audio is native); records `samplerate`. (Android keeps the existing mix-rate check.)
  - On `_exit_tree` (SYNTH): `unregister_worker_ring(&ring)` then the worker teardown (ring is safe to drop after the worker stops).

- [ ] **Step 1: Write the failing tests**

`extension/tests/server_audio_tests.cpp` (device-free via a test seam `void LibpdServer::_set_audio_port_for_test(std::unique_ptr<AudioPort>)` → `audio_open` uses a `NullPort` when set, else `PortAudioPort`):
1. **blocksize not a multiple of 64** — `audio_open(100, 44100)` → false.
2. **audio_open ok** — `audio_open(256, 44100)` → true; `audio_available()==true`; `audio_output_latency_ms()>=0`.
3. **set_mixer wiring** — create a mixer `LibpdInstance`, `init(44100,16,2)` (SYNTH path first is wrong — set role MIXER via the API), load a mix patch, `set_mixer(it)` → true; a synth `init(44100,0,2)` registers its ring. (If constructing `LibpdInstance` in C++ is impractical, pin the blocksize/samplerate contract + the `NativeAudio` register/set_mixer calls directly.)
4. **samplerate mismatch** — a synth `init(48000)` while the stream is 44100 fails (or `NativeAudio` rejects it).

- [ ] **Step 2: Run test to verify it fails**

Re-run the server test build. Expected: FAIL (no `audio_*`/`set_mixer`).

- [ ] **Step 3: Implement the server + instance changes**

Per the interfaces above. `_bind_methods` registers `audio_open`, `audio_close`, `set_mixer`, `audio_list_output_devices`, `audio_list_input_devices`, `audio_output_latency_ms`, `audio_available`. Gate native behavior on `#ifdef NATIVE_AUDIO`.

- [ ] **Step 4: Run test to verify it passes**

Re-run the server test build + full host `ctest`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/libpd_server.h extension/src/libpd_server.cpp extension/src/libpd_instance.h extension/src/libpd_instance.cpp extension/tests/server_audio_tests.cpp
git commit -m "native audio: server audio_* API + set_mixer + instance roles (M5 T6)"
```

---

### Task 7: CMake — PortAudio in place + `NATIVE_AUDIO` + test targets

**Files:**
- Modify: `extension/CMakeLists.txt`

- [ ] **Step 1: Add the PortAudio build + `NATIVE_AUDIO`**

```
if(NOT ANDROID)
    set(NATIVE_AUDIO ON)
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
        target_link_libraries(godot_libpd PRIVATE "-framework CoreAudio" "-framework CoreFoundation" "-framework CoreServices" "-framework AudioToolbox")
    else()
        target_compile_definitions(godot_libpd PRIVATE PA_USE_ALSA=1)
        target_link_libraries(godot_libpd PRIVATE asound)
    endif()
endif()
```

- [ ] **Step 2: Register the new test targets**

Under `if(BUILD_LIBPD_TESTS AND NOT ANDROID)`: `mix_input_ring_tests` (T3, pthread); `native_audio_mixdown_tests` (T4: `tests/native_audio_mixdown_tests.cpp src/core/native_audio.cpp` + `libpd_static` + PortAudio + frameworks + `NATIVE_AUDIO`); `native_worker_tests` (T5: worker + ring + `godot-cpp` + `libpd_static` + `NATIVE_AUDIO`); `server_audio_tests` (T6: server+instance+worker+native_audio+portaudio_port + `godot-cpp` + `libpd_static` + PortAudio + `NATIVE_AUDIO`). Each with `add_test(...)`.

- [ ] **Step 3: Build the full extension + run all host tests**

`./extension/build.sh` then `cd extension/build/cmake-<plat> && ctest --output-on-failure`.
Expected: **all existing tests pass + the new audio tests pass** (or clean `SKIP` where no device).

- [ ] **Step 4: Commit**

```bash
git add extension/CMakeLists.txt
git commit -m "native audio: CMake PortAudio in place + NATIVE_AUDIO + test targets (M5 T7)"
```

---

### Task 8: Test app — 8 synth + 1 mix-down — + macOS on-device verify

The acceptance leg (Spec §10, M5). Prove, on a real Mac, that 8 synth instances mix through the mix-down (EQ/FX) to the native speakers.

**Files:**
- Create: `test_project/scenes/test_native_mix.tscn` + `test_project/scripts/test_native_mix.gd`
- Create: `test_project/data/mixdown_16.pd` (the 16-in/2-out EQ+FX mix patch) + `test_project/data/synth.pd` (a 2-ch synth)

**Interfaces:**
- Consumes: `Libpd` autoload (`audio_available`, `audio_list_output_devices`, `audio_open`, `set_mixer`, `audio_output_latency_ms`), `LibpdInstance`.

- [ ] **Step 1: Write the mix-down + synth patches**

`mixdown_16.pd`: 16 `[adc~ N]` → per-pair `[+~]` (sum L+R of each pair to a mono bus, optional) → an `[eq4~]`/`[phasor~]`-based overall EQ + a `[samplerate~]`-independent FX (e.g. `[*~]` makeup gain) → `[dac~]`. Keep it simple but real: 16 `adc~` → 8 `+~` (pair-sum) → `[*~ 0.5]` (headroom) → a light `[biquad~]`/`[lp2~]` → `dac~`. `synth.pd`: `[osc~ 220] *~ 0.2 dac~` (2 ch).

- [ ] **Step 2: Write the test scene** (GDScript 4.6 — `#` comments only, no `//`)

`test_native_mix.gd`: `_ready`: if `!Libpd.audio_available()` → `push_error`+return. `Libpd.audio_open(256, 44100)` (16-in/2-out default). Create the **mixer** `LibpdInstance` (role=mixer), `init(44100, 16, 2)`, load `mixdown_16.pd`. Create **8** synth `LibpdInstance`s, `init(44100, 0, 2)`, load `synth.pd`. `Libpd.set_mixer(mixer)`. `start_dsp()` on all 9. After 2 s, print `Libpd.audio_output_latency_ms()`; after 5 s, `Libpd.audio_close()`.

- [ ] **Step 3: Build + export + run on the Mac**

Rebuild the extension (Task 7), rebuild the macOS arm64 export (existing custom template), export the test project, run the app. Confirm console: `audio_available()==true`, `audio_open()==true`, `set_mixer()==true`, `audio_output_latency_ms()` printed, **the mixed output is audible** (8 synths through the EQ/FX mix-down).

- [ ] **Step 4: On-device verification (HARD VERIFICATION RULE)**

Capture the console log (paste it). Confirm: no crash over the full 5 s, `set_mixer()==true`, latency printed. Paste the real command + output.

- [ ] **Step 5: Commit**

```bash
git add test_project/scenes/test_native_mix.tscn test_project/scripts/test_native_mix.gd test_project/data/mixdown_16.pd test_project/data/synth.pd
git commit -m "native audio: test app 8-synth + mix-down + macOS verification (M5 T8)"
```

---

## Self-Review (writing-plans checklist)

**1. Spec coverage:** §2 constraint (multi-instance) → Global Constraints + Task 4 test 5 (9-concurrent). §4 architecture → Task 4 (`NativeAudio` mix-down-in-callback) + Task 5 (worker roles) + Task 3 (rings). §4.3 (mix via mix-down) → Task 4 + Task 8 (`mixdown_16.pd`). §3 API (`audio_*` + `set_mixer`) → Task 6. §4.5 (constraint finding) → carried. §6 components → matches the tasks (MixInputRing T3, NativeAudio T4, worker T5, CMake T7). §6.1 block-size/samplerate → Task 4 (`open` validation) + Task 6 test. §6.2 openfile RT-safety → `mix_render_lock` (T4/T5). §6.4 no-device → `NullPort` (T1) used in T4/T6 tests. §10 success → Task 8. **Gap flagged:** Linux/Knulli on-device is not a separate task — it reuses Task 4/7's `ALSA` path (host-gated), with a follow-up verify on a Knulli.

**2. Step scan:** every step pins one action with a checkable result. `NativeAudio::render_block` body given (it's the crux + encodes the no-switching invariant). `MixInputRing` semantics (latest-wins, empty=silence) pinned by tests. Worker role branch given. No "handle edge cases" lines.

**3. Type consistency:** `MixInputRing` (T3) → used in T4 (`register_worker_ring`), T5 (`Config.worker_ring`), T6 (instance creates it). `NativeAudio::set_mixer/register_worker_ring/with_mixer_lock` (T4) → called in T5 (`with_mixer_lock`) + T6 (server). `Config.role/worker_ring/with_mixer_lock` + `pd_instance_ptr()` (T5) → consumed in T6. `LibpdServer::audio_open/set_mixer` (T6) → consumed in T8. Consistent.

**4. Review Focus:** no-switching → T4 (render_block sets instance once; reviewer checks no second call). 9-concurrent → T4 test 5 + T7. Ring latest/underrun → T3 tests 1-5. mix_render_lock → T4 test 4 + T5. set_mixer-before-render → T4 test 3. Block-size/samplerate → T6 tests 1/4.

**5. Proportion:** comparable to the spec's §M5 detail; adds the file-level decomposition + test assertions. Bodies appear only where the invariant/behavior isn't determined by the signature (render_block's set_instance-once + gather + lock; the worker role branch; the CMake PortAudio block). T1/T2 are referenced as done, not re-specced.

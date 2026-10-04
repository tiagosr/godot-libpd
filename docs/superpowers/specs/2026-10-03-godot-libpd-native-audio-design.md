# godot-libpd v2 (M5/M6): Native audio backends (a2) + audio input — Design

Status: **M5 (macOS + Linux) COMPLETE — verified 2026-10-04** (commit
ed2fe5c6e1): 8 kick-driven SYNTH workers + mix-down rendered in the PortAudio
callback, clean summed chord, no crackle, no heap corruption, no atonal hum.
M6 (Android AAudio) pending. Architecture revised 2026-10-03 (Approach A's
single shared RT thread is infeasible for multi-instance — pd global state
breaks on `pd_this` switching; revised to per-instance workers + a mix-down
instance rendered in the PortAudio callback; repro-validated at 9 concurrent
instances). Crackle fixed 2026-10-04 by making the PortAudio callback the
single clock (callback-driven synth kick; see
`2026-10-04-godot-libpd-callback-driven-synth-kick.md`). See §2 constraints,
§4, §4.5, §10.
Supersedes nothing; extends `2026-09-27-godot-libpd-gdextension-design.md`
(the v1 spec listed "native audio backends a2: CoreAudio / OpenSL ES / ALSA"
and "audio input / microphone capture" as v2 items, and left the sink behind
the `PdAudioSink` interface for exactly this swap).

Chosen approach: **PortAudio** as the native backend (device stream +
latency/enumeration). Per-instance libpd DSP runs on the **existing a1 worker
threads** (one pinned audio thread per instance); the overall **EQ+FX mix-down
is a dedicated libpd instance rendered in PortAudio's real-time stream
callback** (§4). **AAudio/Oboe is integrated as a new PortAudio hostapi
backend** to cover Android (M6).

## 1. Purpose & success criteria

Replace the Godot-native `AudioStreamGenerator` sink (v1 "a1") with a native
audio backend, to unlock the three capabilities v1 deliberately deferred:

- **Controllable block size** — the app picks the device buffer (frames per
  callback), not Godot's AudioServer. This is the primary latency knob.
- **Reduced latency** — one clock (the device callback) instead of a
  self-paced worker thread + a ~250 ms ring buffer + a per-frame main-thread
  pump + a float→Vector2 conversion.
- **Audio input** — `[adc~]` works: device capture feeds
  `libpd_process_float(ticks, in, out)`'s `in`.

Backends, split by what is available in the vendored PortAudio:

- **M5 (core):** PortAudio on **macOS (CoreAudio)** and **Linux (ALSA)**.
  Full Approach A. `AudioStreamGenerator` sink is retired on these platforms.
- **M6 (Android):** a **new PortAudio hostapi backend over AAudio (or Oboe)**
  so Approach A works on the RG DS. Until M6 lands, Android keeps the v1
  `AudioStreamGenerator` sink — no audio regression.

Success: a patch with `[adc~]`→`[dac~]` (or any synth) renders in
real time on macOS and the Knulli with no ring buffer; a chosen block size
(e.g. 128) is honored; measured round-trip latency drops from the v1
~10–20 ms+ to the device-block + driver floor; the `audio_open()` block-size
parameter is respected; and (M6) the same path works on the RG DS via AAudio.

**Out of scope (this effort):** sample-accurate / guaranteed-latency contracts
(v1 §11 unchanged); per-instance independent output devices (one shared
device, mixed — §4); exclusive/WASAPI-only or ASIO tuning as a *requirement*
(PortAudio exposes WASAPI-exclusive + optional ASIO if ever wanted); iOS
(AVAudioEngine) — remains a separate future item; multi-channel layouts
beyond the common 2-in / 2-out (remap supports arbitrary counts, §4.4, but
the API surfaces are untested beyond that).

## 2. Facts established during exploration

- **Current a1 sink** (`core/pd_audio_sink.h`, `pd_audio_sink_generator.*`,
  `libpd_worker.cpp`): the per-instance **worker thread** renders one
  `libpd_blocksize()` block, `sleep_until`s to the next tick
  (`blocksize/samplerate`), and `push_block`s interleaved floats into a
  mutex ring sized ~`samplerate/4` (~250 ms). The **main thread**
  (`LibpdInstance::_process`) pumps the ring into
  `AudioStreamGeneratorPlayback::push_buffer()` (converted to `Vector2`
  stereo). Output-only; `n_ins` is effectively unused (`in=nullptr`).
- **libpd is built to run in a real-time callback.** The API
  (`thirdparty/libpd/libpd_wrapper/z_libpd.h`):
  - `libpd_init_audio(nIn, nOut, sampleRate)` — per-instance audio config.
  - `libpd_process_float(ticks, inBuffer, outBuffer)` — interleaved float,
    reads `nIn` ch from `in`, writes `nOut` ch to `out`; `ticks` blocks per
    call, each block = `libpd_blocksize()` frames.
  - `libpd_blocksize()` == `DEFDACBLKSIZE` == **64** (pd~.c). So the device
    buffer **must be a multiple of 64** for a clean zero-copy callback
    (`ticks = framesPerBuffer / 64`).
- **Canonical reference in-tree:**
  `thirdparty/libpd/samples/c/pdtest_portaudio/pdtest_portaudio.c` is
  exactly the Approach A pattern:
  ```c
  static int pa_callback(const void *in, void *out, unsigned long frames, ...) {
      int ticks = frames / libpd_blocksize();
      libpd_process_float(ticks, in, out);
      return 0;
  }
  ```
  with `Pa_OpenStream(... paFloat32, 44100, 512, pa_callback, ...)` and the
  stream passed straight into `libpd_process_float` (no copy). This validates
  the whole model end-to-end against the real libpd we ship.
- **PortAudio is already vendored** at
  `thirdparty/libpd/pure-data/portaudio/portaudio/` (include/, src/common,
  src/hostapi, src/os; BSD-style license). Its `src/hostapi/` ships
  **`alsa, asio, coreaudio, wasapi, wmme` only — no Android/OpenSL backend.**
  Mainline PortAudio likewise has no official Android hostapi. Therefore the
  desktop backends (CoreAudio, ALSA) are usable today, and **Android needs a
  new hostapi backend (AAudio/Oboe)** — the M6 workstream.
- **Android reality:** OpenSL ES is **deprecated**; Google's current
  recommendation is **Oboe** (C++ wrapper; calls AAudio, falls back to
  OpenSL ES). AAudio's C API is available from API 28+ (our minSdk is 29).
- **v1 fail-fast** on `AudioServer.get_mix_rate() != instance samplerate`
  goes away with native audio (no more Godot mix-rate coupling); the
  constraint becomes "instance samplerate == open stream sample rate."
- **Multi-instance — HARD CONSTRAINT (repro-confirmed 2026-10-03):**
  `libpd_set_instance()` is a thread-local (`pd_this`), but pd's **global**
  dsp/scheduler state cannot survive *switching* `pd_this` between live
  instances mid-dsp. A shared thread doing `set_instance(A); process_float;
  set_instance(B); process_float` **crashes** (`EXC_BAD_ACCESS`) with ≥2
  instances — even 2 merely existing (one rendered) crashes. **One dedicated
  thread per instance** (each `set_instance` once, then loop) supports **9+
  concurrent instances** (repro: 8 stereo synth workers + 16-in/2-out mix-down
  on a separate render thread, no crash). So the audio model is **one audio
  thread per instance** (the a1 worker model) + a **mix-down instance** on the
  PortAudio callback thread (§4). Corroborated: "libpd doesn't support multiple
  instances because there are global variables" + libpd#406. Repros:
  `spike/mi_*`.

## 3. Public API (GDScript)

Audio is a **server-level** concern (one shared stream/device), so device and
block-size control live on the `Libpd` singleton; per-instance channel layout
stays on `LibpdInstance`. The v1 per-instance `init(samplerate, n_ins, n_out)`
signature is preserved; `n_ins > 0` now means "this instance reads the device
input."

```gdscript
# ---- audio stream (server-level, on Libpd) ----
Libpd.audio_available() -> bool                 # false if no usable device
Libpd.audio_list_outputs() -> Array             # [{index, name, max_ch}]
Libpd.audio_list_inputs() -> Array
Libpd.audio_open(output_index: int,
                 input_index: int = -1,         # -1 = output-only
                 blocksize: int = 256,          # frames; multiple of 64
                 sample_rate: int = 0) -> bool  # 0 = device default
Libpd.audio_close() -> void
Libpd.audio_blocksize() -> int
Libpd.audio_sample_rate() -> int
Libpd.audio_input_enabled() -> bool
Libpd.audio_latency_estimate_ms() -> float      # device-block + driver floor
Libpd.set_mixer(instance: LibpdInstance) -> bool
    # Designate the mix-down instance (a 16-in/2-out patch: [adc~ 1..16] ->
    # EQ+FX -> [dac~]). It is rendered in the PortAudio callback; its 16
    # inputs are the 8 synth workers' stereo pairs. All other instances are
    # synth workers feeding their MixInputRing. Must be called before the
    # mixer's start_dsp(); re-designating swaps the callback's target.

# ---- per instance (LibpdInstance) — v1 signature, now wired to the stream ----
LibpdInstance.init(samplerate: int = 44100, n_ins: int = 0, n_out: int = 2) -> bool
    # samplerate MUST equal Libpd.audio_sample_rate() (fail-fast, §7.1).
    # n_ins > 0 → instance reads the device input ([adc~]).
```

Semantics:

- `audio_open()` opens the **shared** real-time stream and starts it. It is
  idempotent-ish: re-opening closes first. Opening is a no-op error if
  `audio_available()` is false (headless / no device).
- **Block-size contract:** `blocksize` is clamped to the nearest multiple of
  `libpd_blocksize()` (64) and to the device's min/max (queried at open).
  The effective value is what `audio_blocksize()` returns. Smaller = lower
  latency, higher CPU. Default 256 (~5.8 ms @ 44.1 kHz).
- **Input:** `audio_open(..., input_index >= 0)` enables capture. Every
  instance with `n_ins > 0` reads the *same* device input (the stream's input
  buffer); instances with `n_ins == 0` contribute no input. This matches
  "[adc~] reads the hardware" — there is no per-instance input routing.
- **Sample rate:** one rate for the whole stream. Instances must match it
  (fail-fast); no built-in resampling (out of scope, §1).
- **Device selection:** by index from `audio_list_*`. `-1`/`0` = the
  platform default. (Per-device channel remap is §4.4.)
- Android before M6: `audio_available()` is false and `audio_open()` no-ops;
  the instance falls back to the v1 `AudioStreamGenerator` sink (a1), so
  existing Android behavior is unchanged. (The a1 path stays compiled until
  M6 retires it.)

## 4. Architecture & threading — per-instance workers + mix-down in the audio callback

**Constraint that shapes the whole design (repro-confirmed 2026-10-03):**
libpd's `libpd_set_instance()` sets a thread-local `pd_this`, but pd's
**global** dsp/scheduler state cannot survive *switching* `pd_this` between
instances mid-dsp. Reproduced: a shared thread doing
`set_instance(A); process_float; set_instance(B); process_float` **crashes**
(`EXC_BAD_ACCESS`) with ≥2 live instances — even 2 instances merely *existing*
(only one rendered) crashes. Conversely, **one dedicated thread per instance**
(each calls `set_instance` exactly once, then loops `process_float`) supports
**9+ concurrent instances** cleanly. Corroborated upstream ("libpd doesn't
support multiple instances because there are global variables"; libpd#406).

**Consequence:** the original "Approach A" (one shared RT thread rendering
*all* instances) is **infeasible for multi-instance**. The adopted model keeps
**one audio thread per instance** — exactly v1's a1 worker-thread model — and
moves the *mix* into a dedicated **mix-down libpd instance** rendered on the
PortAudio callback thread.

```
Synth worker (per instance, e.g. 8)  (EXISTING a1 worker — rendering KEPT):
    its OWN dedicated thread, pinned: libpd_set_instance(i) ONCE (in INIT),
    then loop:  libpd_process_float(1, nullptr, out2ch)   (paced to sample rate)
                push out2ch -> MixInputRing_i             (lock-free ring, 2ch)
    Renders its STEREO PAIR; does NOT touch the device or other instances.

PortAudio callback thread  (the ONLY thread that renders the MIX-DOWN):
    pinned to the mix-down: libpd_set_instance(mix) ONCE (at stream start):
        gather latest 2ch from each MixInputRing_i -> mix_in[16ch]   (8 pairs)
        libpd_process_float(ticks, mix_in, mix_out)                  # EQ+FX mix
        copy mix_out[2ch] -> PortAudio outputBuffer                  # to device
    No allocation / I/O / Godot / blocking beyond the fixed-buffer gather.

MIX-DOWN instance  (a normal LibpdInstance, role = mixer):
    its worker thread is CONTROL-ONLY (INIT/LOAD/UNLOAD/MESSAGE; no render
    loop — the PortAudio callback renders it). Loaded with a pd patch:
    [adc~ 1..16] -> EQ + FX -> [dac~]   (16 in = 8 stereo pairs; 2 out).

Main thread
    Godot UI; enqueues to workers / server; drains event/MIDI rings; emits
    signals. Does NOT render audio.
```

Invariants:

- **One audio thread per instance.** Each instance's `libpd_process_float`
  runs on exactly one thread that `libpd_set_instance()`d it once. The 8
  synth workers + the mix-down (on the callback thread) = 9 pinned threads.
  **No thread ever switches `pd_this` between instances** — that is the
  crash (constraint at the top of §4).
- **No cross-thread render of one instance.** For each *synth* instance, both
  its control ops and its render live on its single worker thread (no lock
  needed). The *mix-down* is the exception: its control ops run on its worker
  thread and its render on the PortAudio callback thread, serialized by a
  single `mix_render_lock` (a brief stall while the mix patch loads — §7.2).
- **`pd_this` is thread-local (spike-confirmed).** pd's current-instance
  pointer is a `PERTHREAD` global (`m_class.c:34`). **Every thread that
  calls `libpd_*` for an instance calls `libpd_set_instance(it)` exactly
  once, on that thread** — synth workers do it in INIT; the PortAudio
  callback does it for the mix-down once at stream start. This is why v1
  worked (`set_instance` + `process_float` on the same worker thread) and why
  a shared thread switching instances segfaults.
- **Ring handoff.** Each synth worker pushes its block into its
  `MixInputRing`; the callback gathers the latest block from each. A worker
  slightly behind yields a stale (≤1 block) sample — acceptable for a
  mixdown. Rings are sized ≥8 blocks (~11.6 ms @ 44.1 k) so steady state has
  no underrun.
- **Real-time discipline.** The callback does only: a fixed-buffer gather (no
  allocation), one `libpd_process_float`, one copy. No I/O, no Godot/MIDI,
  no blocking. Synth workers are paced (not real-time), so their pacing
  imprecision is absorbed by the rings. (Enforced by code review + §8.)
- **Idle = silent.** The stream stays open (server-owned, §7.3); workers
  that are not ready contribute silence to their rings, so the mixdown just
  sums whatever is present, and the first `init()`+`load()` makes sound with
  no stream churn.

### 4.1 Why per-instance workers (not a single shared RT thread)
The "single shared RT thread, `set_instance` per instance" model (the original
Approach A) is **infeasible for multi-instance**: pd's global dsp/scheduler
state does not survive switching `pd_this` between instances (repro-confirmed,
crashes — see the constraint at the top of §4). **One audio thread per
instance** (the a1 worker model) is the only multi-instance-capable shape, and
it is exactly what the requirement asked for ("worker-thread DSP **per
instance** and multi-instance support"). The mix moves into a dedicated
mix-down instance on the callback thread, so we still get one native device
stream + pd-native EQ/FX without the forbidden instance-switching. The cost vs
Approach A: a small per-worker ring + the mix-down instance — both bounded and
testable.

### 4.2 libpd call mapping
- v1 `libpd_process_float(1, nullptr, out)` (worker, paced) →
  A `libpd_process_float(ticks, in, out)` (audio thread, device-paced).
- `libpd_init_audio` moves from the worker's INIT to the control thread's INIT
  (same call, now run off the audio thread).
- All `libpd_*` message/MIDI calls stay on the control thread (unchanged from
  v1 — the "pd-on-worker" contract becomes "pd-on-control-thread").
- Output hooks (`[noteout]`/`[ctlout]`/`[midiout]`/…) still fire **on the
  audio thread** (they run inside `libpd_process_float`). Their
  `push()` into the per-instance MIDI output queue must stay
  **non-allocating, non-blocking** (bounded ring, drop-oldest) — this is a
  hardening requirement for Approach A (v1 ran hooks on the worker, which
  was never real-time; now it is). §7.2.

### 4.3 Multi-instance → one device (via the mix-down)
One PortAudio stream, one physical output. The 8 synth workers render into
their rings; the **mix-down instance** (a pd patch with 16 audio inputs) takes
all 8 stereo pairs, applies the overall **EQ + FX**, and produces the
2-channel mix that the callback writes to the device. The *mix* is done in pd
by the mix-down patch (the intended design), so channel layout, EQ, and FX are
all patch-level, not C. The C side only *gathers* the worker stereo pairs into
the 16-ch input and hands the 2-ch output to the device. A no-mixer
single-instance mode (one instance's output straight to the device) is the
fallback for apps that don't use a mixdown.

### 4.4 Channel remap (device layout ≠ instance layout)
The stream has a fixed channel count (e.g. 2 in / 2 out); each instance asks
for its own `n_ins`/`n_out`. The audio thread copies the first
`min(deviceCh, instCh)` channels and pads the rest with silence. The common
case (`n_ins==device_in_ch`, `n_out==2`) is a straight copy / zero-copy
input pass. Input is always shared (the same device `in` feeds every
instance); output is per-instance then mixed.

### 4.5 Per-thread instance selection + spike results

**`libpd_set_instance` must be called on every thread before any
`libpd_*` call** (see the invariant above). Concretely:
- audio thread: `libpd_set_instance(i)` per instance, inside the render
  loop (it is cheap — it sets a thread-local pointer).
- control thread: `libpd_set_instance(i)` at the start of each command it
  executes (batch the queue drain per instance, set once).
- output hooks (fire on the audio thread during `process_float`): `pd_this`
  is already set to the instance being rendered, so `libpd_get_instancedata()`
  resolves to that instance's context with no extra work.

**Spike results (macOS, vendored PortAudio 19.7.0 + `libpd-multi.a`,
`spike/native_audio/`):**
- Approach A works: a sine patch rendered in the callback at exactly its
  designed peak; a running `adc~`-fed mix drove the output (input → libpd →
  output confirmed).
- Block-size control works: requested 128 → callback `framesPerBuffer==128`,
  device output latency 22.7 ms; requested 256 → `framesPerBuffer==256`,
  28.5 ms. Floor ≈ device latency + one buffer (2.9 ms @128 / 5.8 ms @256).
- The one real bug found: calling `libpd_process_float` on the audio thread
  without `libpd_set_instance` segfaulted in pd's `sys_lock` (thread-local
  `pd_this` was null). Setting it per-thread fixed it — the core of Approach A.
- Build notes for the plan: PortAudio's CoreAudio backend needs
  `-framework AudioToolbox` (not just CoreAudio) and `-DPA_USE_COREAUDIO=1`
  so `pa_unix_hostapis.c` registers it. The audio input object in this pd
  build is **`adc~`** (1-indexed); there is no `audioin~` class.
- **Multi-instance constraint (post-spike repro, 2026-10-03):** the single
  shared-RT-thread model above is infeasible for multi-instance (pd global
  state breaks on `pd_this` switching). The adopted model (one audio thread
  per instance + mix-down on the callback) was repro-validated at **9
  concurrent instances** (8 stereo synth workers + 16-in/2-out mix-down on a
  separate render thread, ~1.5 s, no crash). Repro binaries: `spike/mi_*`.
  **Status: this revised §4 supersedes the original Approach-A diagram.**

## 5. Backends & platform matrix

| Platform | Milestone | PortAudio hostapi | Notes |
|---|---|---|---|
| macOS | **M5** | `coreaudio` (present) | low latency; default input = built-in mic |
| Linux / Knulli (arm64) | **M5** | `alsa` (present) | codec is typically 8 kHz mono → block size bounded by HW period; measure on-device (§8.3) |
| Android / RG DS | **M6** | **new `aaudio` hostapi** (AAudio C API, or Oboe underneath) | AAudio from API 28+ (minSdk 29); the "OpenSL" v1 item is superseded — PortAudio has no OpenSL hostapi, and AAudio is the modern low-latency path |
| Windows | future | `wasapi` (+ optional `asio`) (present) | not a target this effort; available for free if wanted |

**M6 backend shape:** a new PortAudio hostapi under
`src/hostapi/aaudio/` implementing the `PaHostApi` vtable (init/teardown,
device enumeration via `AAudio_*_Availability`/`AAudio_get*`, open/close/
start/stop stream, buffer-size/latency queries, and the
`AAudioStream_dataCallback` adapted to the PortAudio `PaStreamCallback`).
Two implementation choices (decide in the plan, §9):
- **AAudio C API directly** — lighter, no new dep, matches PortAudio's C
  codebase; we own the buffer/failure handling.
- **Oboe underneath** — vendor Oboe (Google-maintained C++); get its
  robustness + OpenSL fallback for free; slightly more to vendor.
Default recommendation: **AAudio C API directly** (PortAudio is C; keep the
dependency surface minimal), with Oboe as the fallback if AAudio edge cases
bite on the RG DS.

## 6. Components & files

New (`extension/`):

- `src/core/audio_port.h` + `src/core/null_port.h` — **(DONE, T1)** the
  `AudioPort` abstraction (device enum / open / latency + a
  `std::function<void(float*,float*,int)>` render callback), the pure
  `mix_block` (sum+clamp; the no-mixer single-instance fallback), and
  `NullPort` (device-free fake audio thread for host tests).
- `src/core/portaudio_port.{h,cpp}` — **(DONE, T2)** the real PortAudio backend
  (CoreAudio/ALSA): device enumeration, full-duplex stream, the static C
  callback trampoline → the render `std::function`, latency from
  `PaStreamInfo`. Rejects `blocksize % 64 != 0`.
- `src/core/mix_input_ring.h/.cpp` — **(new)** per-synth-worker lock-free ring
  (2 ch, ≥8 blocks). `push()` from the worker's render; `gather_latest()` from
  the PortAudio callback. Producer = worker thread, consumer = callback thread.
- `src/core/native_audio.{h,cpp}` — **(revised)** server-owned. Owns the
  `PortAudioPort` stream + the **mix-down render-in-callback**: registers the
  mix-down `t_pdinstance*` + the list of synth `MixInputRing`s. Callback body:
  gather the latest 2ch from each ring → 16 ch, `libpd_set_instance(mix)` once,
  `libpd_process_float(ticks, in16, out2)`, copy `out2` → device. Also
  `mix_render_lock` (serializes the mix-down's worker-thread control ops with
  the callback render). No-device fallback via `NullPort`.
- `tests/` — host tests: `mix_input_ring` (producer/consumer); `native_audio`
  gather + mix-down render (NullPort + a real libpd mix instance, 16-in/2-out);
  and the 9-concurrent-instance scenario (8 workers + mix-down) as a regression
  test.

Modified:

- `src/libpd_worker.{h,cpp}` — **synth worker keeps the a1 render loop**
  (`libpd_process_float` + `sleep_until` pacing) but pushes each block to its
  `MixInputRing` (new sink) instead of the `AudioStreamGenerator`. `Config`
  gains a role flag (**synth** / **mixer**) + the ring pointer. The **mixer**
  worker is **control-only** (no render loop — the PortAudio callback renders
  it); its control ops (INIT/LOAD/UNLOAD) acquire `NativeAudio`'s
  `mix_render_lock`. Output-hook `push()` stays non-blocking (§4.2).
- `src/libpd_instance.{h,cpp}` — **synth** instance: worker + its
  `MixInputRing`. **Mixer** instance: designated (role=mixer), control-only
  worker, registered with `NativeAudio` for callback rendering. The
  `AudioStreamGenerator`/`GeneratorSink`/`_process` pump is **retired on
  desktop** (M5) — kept behind a compile flag for the Android a1 fallback
  until M6.
- `src/libpd_server.{h,cpp}` — own `NativeAudio`; expose the `audio_*` API
  (§3) + `set_mixer(instance)`; wire synth-ring registration + mixer
  registration into the instance lifecycle; shutdown order (§7.3).
- `src/core/pd_audio_sink.h` / `pd_audio_sink_generator.*` — **retired** on
  desktop (M5); kept as the Android a1 fallback until M6, then removed.
- `CMakeLists.txt` — build PortAudio **in place** from
  `thirdparty/libpd/pure-data/portaudio/portaudio`; a `NATIVE_AUDIO` option
  (default ON, OFF on Android); per-platform link (CoreAudio/ALSA); the new
  test targets. M6: an `AUDIO_AAUDIO` option adding the aaudio hostapi +
  AAudio lib on Android.

## 7. Lifecycle, errors, shutdown

- **7.1 Fail-fast on rate.** `init()` requires `samplerate ==
  audio_sample_rate()` (else `ERR_INVALID_PARAMETER` + `_emit_failure`, same
  shape as v1). No resampling (out of scope).
- **7.2 openfile real-time safety (the main gotcha).** `libpd_openfile`
  does file I/O + graph allocation → must not run on the audio thread.
  Default: the control thread runs it under `i.audio_mutex`; the audio
  thread holds that lock for one render block, so a load briefly stalls
  audio (a one-time glitch on "load patch" — acceptable, documented).
  Glitch-free alternative (optional, plan decides): build the new pd graph
  on a scratch thread, then swap the callback's target instance under the
  lock — natural given we already use `libpd_new_instance`/`set_instance`.
  **Hard requirement either way:** the audio thread never performs I/O; it
  only ever waits (bounded) on the mutex, and the mutex hold on the audio
  side is always O(one render block).
- **7.3 Stream lifecycle (server-owned).** The stream opens once
  (`audio_open`) and closes on `audio_close` / server deinit — independent
  of instance add/remove, so there is no open/close churn or per-instance
  glitch. Refcount-based open-on-first-init is a possible optimization, not
  required. On **no device** (`audio_available()==false`, headless/tests),
  `audio_open` no-ops and instances use the NullBackend (counts frames,
  tracks peak) — this preserves headless tests and a Knulli where ALSA
  fails.
- **7.4 Instance freed while streaming.** Control-thread teardown
  unregisters the instance from the audio object (registry ops are
  mutex-guarded; the audio thread sees the change next callback) and frees
  `out_buf`. No audio-thread teardown of pd (the control thread owns pd
  lifetime, as in v1).
- **7.5 Error model.** `audio_open` returns false + `push_error` on failure
  (no device, bad blocksize after clamping still infeasible, etc.). Runtime
  stream errors (e.g. xrun) are logged via `pd_dbg` and surfaced as a
  one-time warning, not fatal. No asserts on user paths (v1 §10).
- **7.6 Shutdown order** (server deinit): `audio_close` (stop+close the
  stream, join the audio thread) → (instances' control threads tear down
  pd as before) → clear registry. Audio thread joined **before** any pd
  instance is freed, so no `process_float` can race a `free_instance`.

## 8. Testing

1. **Host unit tests** (NullBackend, no device — `native_audio_mix_tests`):
   - block-size alignment/clamp to a multiple of 64 + device min/max;
     `ticks = frames/64`.
   - channel remap (§4.4): 2ch device → 1ch instance, 1ch → 2ch, 2→2
     zero-copy.
   - multi-instance mix (§4.3): N instances sum correctly; clamp at
     [-1,1]; one instance silent when not ready.
   - the non-blocking output-hook `push` (§4.2): a synthetic `[noteout]`
     burst doesn't block / allocate in a way the audio thread could stall on.
2. **macOS integration (automated, host):** open the stream (built-in output,
   blocksize 128), load a synth patch, assert `blocks_rendered` advances and
   peak > 0 through the NullBackend-instrumented path or a loopback; a
   `[adc~]`→`[dac~]` patch with the built-in mic proves the **input**
   path (peak > 0 on the output ring).
3. **Knulli on-device (manual):** run the app, `audio_open(blocksize=128)`;
   verify sound from the 3.5 mm jack, confirm `audio_blocksize()==128`, and
   record the observed latency floor (the §9 "is the Knulli codec the
   bottleneck" question — 8 kHz mono caps real latency).
4. **RG DS on-device (M6, manual):** same app; the aaudio backend renders a
   synth + a `[adc~]`→`[dac~]` loopback; confirm no OpenSL path and
   a lower latency than v1's a1 sink.
5. **Test app** (`test_project/scenes/test_audio_native.tscn` + script):
   device lists, `audio_open` with a block-size slider (128/256/512/1024),
   enable-input toggle, live peak/latency `Label` (A133 RichTextLabel
   lesson), a loopback patch button, and a v1-vs-native peak comparison.

## 9. Milestones & a de-risking spike

- **Spike (before full SDD, ~half a day):** a minimal `NativeAudio` that
  opens a PortAudio duplex stream at a chosen blocksize, renders *one*
  existing patch via the §4 callback, and prints peak + a latency estimate.
  Validate on macOS: the callback model, block-size control, and input all
  work; measure the latency floor. This de-risks §4/§7 before we commit the
  worker refactor, and answers the Knulli-codec question early.
- **M5 (core):** the full Approach A on macOS + Linux per §4–§7; retire the
  a1 sink on desktop; host tests + macOS integration + Knulli on-device.
  Android keeps a1 (no regression).
- **M6 (Android):** the new AAudio PortAudio hostapi (§5) → Approach A on
  the RG DS; then retire the a1 sink everywhere.

## 10. Risks & open notes

- **Worker refactor size (reduced)** — the synth workers **keep** the a1
  render loop; the change is (a) pointing their sink at a `MixInputRing`
  instead of the generator, (b) a control-only **mixer** role, and (c) the
  `NativeAudio` mix-down-in-callback object. The per-instance pinned-thread
  render is the same proven v1 pattern, so the refactor is smaller than the
  original Approach-A plan implied.
- **openfile glitch (§7.2)** — the default (brief audio stall on load) is
  acceptable; the scratch-swap alternative is more code. Decide in the plan
  after seeing how often loads happen in the real app.
- **Multi-instance audio (RESOLVED design constraint)** — repro-confirmed:
  a shared RT thread switching `pd_this` between live libpd instances crashes
  (`EXC_BAD_ACCESS`); one dedicated thread per instance (the a1 model)
  supports 9+ concurrent instances. M5 uses per-instance workers + a mix-down
  instance on the PortAudio callback (no instance-switching). The 9-instance
  scenario is a regression test. Residual: the mix-down's
  control-op-vs-callback-render serialization (`mix_render_lock`) — a brief
  stall while the mix patch loads, acceptable (§7.2).
- **Output-hook hardening (§4.2)** — synth-worker hooks fire on paced worker
  threads (never real-time, as in v1 — low risk); the **mix-down's** hooks
  fire on the PortAudio callback thread (real-time), so that path's
  `push()` must be non-blocking/non-allocating. Covered by a host test.
- **Knulli codec ceiling** — an 8 kHz mono codec means "reduced latency"
  there is bounded by hardware, not the lib. The spike + §8.3 confirm the
  real number so we don't chase a non-existent win on that device.
- **M6 AAudio backend effort** — a full `PaHostApi` is real C work
  (enumeration, open/stream, callback adaptation). AAudio C API directly is
  the leaner default; Oboe is the robustness fallback. This is separable:
  M5 ships fully without it.
- **PortAudio version** — the bundled copy predates a version macro I could
  confirm; the plan pins a known-good release when lifting it to
  `thirdparty/portaudio` (and re-checks CoreAudio/ALSA hostapi compatibility).
- **iOS** stays out of scope (separate future item, as in v1 §11).

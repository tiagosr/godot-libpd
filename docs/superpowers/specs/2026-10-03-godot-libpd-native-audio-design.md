# godot-libpd v2 (M5/M6): Native audio backends (a2) + audio input — Design

Status: proposed (Approach A + PortAudio spike validated on macOS 2026-10-03 — see §2 facts and §4.5)
Supersedes nothing; extends `2026-09-27-godot-libpd-gdextension-design.md`
(the v1 spec listed "native audio backends a2: CoreAudio / OpenSL ES / ALSA"
and "audio input / microphone capture" as v2 items, and left the sink behind
the `PdAudioSink` interface for exactly this swap).

Chosen approach: **PortAudio** as the native backend, with the libpd DSP
running **inside PortAudio's real-time stream callback** ("Approach A" — see
§4). **AAudio/Oboe is integrated as a new PortAudio hostapi backend** to cover
Android (M6).

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
- **Multi-instance** (v1 requirement): each `LibpdInstance` owns one pd
  instance + one worker thread. Under Approach A there is **one** hardware
  output device, so all instances are rendered and mixed on one shared
  real-time thread (§4.3).

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

## 4. Architecture & threading — Approach A

libpd's DSP runs **on the single real-time audio thread** (the PortAudio
stream callback). The worker threads become pure **control** threads. This is
the model that delivers the §1 goals: one clock, no ring, direct device
buffers, input for free.

```
Real-time audio thread  (NEW, owned by LibpdServer; the ONLY thread that
    calls libpd_process_float). PortAudio stream callback, ~128..1024
    frames per invocation:

    frames = stream block size            (multiple of libpd_blocksize()=64)
    ticks  = frames / libpd_blocksize()
    clear  dev_out[]                       (device output scratch)

    for each active instance i (server registry, fixed order):
        if i is not ready (no patch / dsp off): continue   (contributes 0)
        lock   i.audio_mutex               (brief — one render block)
        libpd_set_instance(i)              (pd_this is THREAD-LOCAL — §4.5)
        remap  dev_in  -> i.in_buf         (§4.4, zero-copy when n_ins==dev_in_ch)
        libpd_process_float(ticks, i.in_buf, i.out_buf)    # renders i
        dev_out[] += i.out_buf               (mix; scale to avoid overflow)
        unlock i.audio_mutex

    copy dev_out[] -> PortAudio outputBuffer
    (PortAudio inputBuffer is dev_in; it is already the device capture)

    NO allocation, NO file I/O, NO Godot calls, no MIDI, no blocking.

Per-instance control thread  (EXISTING worker, repurposed — no longer does
    DSP / pacing). Drains the instance command queue:
    INIT   -> libpd_new_instance / libpd_set_instance /
              libpd_init_audio(n_ins, n_out, stream_rate) / install hooks
    LOAD   -> libpd_openfile        (file I/O + graph alloc; NOT real-time)
    UNLOAD -> libpd_closefile
    MESSAGE / MIDI_* -> libpd_message / libpd_noteon / ...   (cheap)
    Serialized against the audio thread by the SAME i.audio_mutex (§7.2).

Main thread
    Godot UI; LibpdInstance/Libpd method calls enqueue to control threads or
    the server; drains the (unchanged) main-thread event/MIDI rings; emits
    signals. Does NOT render audio.
```

Invariants:

- **One clock.** The device callback is the only timing source; the v1
  `sleep_until` pacing loop and the ~250 ms output ring are deleted.
- **No cross-thread libpd.** For a given pd instance, `libpd_process_float`
  (audio thread) and every control op (control thread) are serialized by
  `i.audio_mutex`. Two threads never touch one instance simultaneously.
- **`pd_this` is thread-local (spike-confirmed).** pd's current-instance
  pointer is a `PERTHREAD` global (`m_class.c:34`), so an instance is only
  visible to the thread that `libpd_set_instance()`d it. **Every thread that
  calls `libpd_*` for instance i must call `libpd_set_instance(i)` first** —
  the audio thread does it per-instance inside the render loop (§4.5), the
  control thread does it once per command batch. This is why v1 worked
  (`set_instance` + `process_float` on the same worker thread) and why a
  naive "`process_float` on the audio thread" segfaults in `sys_lock`.
- **Real-time discipline.** The audio thread does only: a lock, one
  `libpd_process_float` per instance, a mix, an unlock. No allocation, no
  I/O, no Godot/MIDI calls, nothing that can block. (Enforced by code
  review + the §8 latency test.)
- **Idle = silent.** The stream stays open (server-owned, §7.3); instances
  that are not ready contribute silence, so an empty app just plays noise
  floor, and the first `init()`+`load()` makes sound with no stream churn.

### 4.1 Why not the v1 worker-as-clock model
Keeping the worker rendering into a ring that a native callback drains
("Approach B") is lower-risk but caps the win: it keeps two clocks and an
output ring (the very indirection we are removing), and adds a *second* ring
for input. Approach A removes all of it. The cost is the worker refactor and
the openfile serialization in §7.2 — both bounded and testable.

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

### 4.3 Multi-instance → one device
One PortAudio stream, one physical output. The audio thread renders each
active instance into its own `out_buf` and **sums** into `dev_out` (with
normalization: divide by active count, or clamp — spec default: clamp to
[-1,1], document). This is what "multi-instance support" means at the audio
layer (v1 got this for free from Godot mixing one player per node; now we
mix explicitly).

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

- `src/audio/native_audio.h/.cpp` — the shared real-time audio object
  (owned by `LibpdServer`): owns the PortAudio stream + callback, the
  active-instance registry, `dev_in`/`dev_out` scratch, the per-instance
  `audio_mutex` handle, block-size/sample-rate state, device enumeration, and
  the no-device fallback (dry). Exposes: `open/out/in/blocksize`,
  `register_instance/unregister_instance`, `render_callback()`.
- `src/audio/pa_compat.h` — thin wrapper over `portaudio.h` so the dry
  fallback and the tests can run without a device (a "NullBackend" that
  counts frames + tracks peak, mirroring v1 `DrySink`).
- `thirdparty/portaudio/` — PortAudio as a **top-level** thirdparty
  (lifted from the libpd-bundled copy; keeps the libpd submodule untouched).
  M6 adds `src/hostapi/aaudio/` here. (Alternative: compile in place from
  `thirdparty/libpd/pure-data/portaudio/portaudio`; decide in plan.)
- `tests/native_audio_mix_tests.cpp` — host tests for the **platform-
  independent** logic: block-size alignment/clamping, the channel remap
  (§4.4), the multi-instance mix/clamp (§4.3), and the `ticks` computation.
  These run with the NullBackend (no device needed).

Modified:

- `src/libpd_worker.{h,cpp}` — **repurpose the worker**: delete the
  `libpd_process_float` + `sleep_until` pacing from `run()`; the worker
  becomes a pure command-dispatch thread. `Config` drops `sink`; gains a
  pointer to the shared audio object (to register `out_buf` + `audio_mutex`)
  and the `n_ins`/`n_out`/rate it reports to `libpd_init_audio`. Output-hook
  `push()` hardened to non-blocking (§4.2).
- `src/libpd_instance.{h,cpp}` — `init()` now registers with the shared
  audio object (allocate `out_buf`, join `audio_mutex`, report `n_ins`/
  `n_out`/rate); the `AudioStreamGenerator`/`GeneratorSink`/`_process` pump
  is **retired** (M5) — kept behind a compile flag for the Android a1
  fallback until M6. `debug_sink_peak` repointed at the audio object.
- `src/libpd_server.{h,cpp}` — own the `NativeAudio` object; expose the
  `audio_*` API (§3); wire instance register/unregister into its lifecycle;
  shutdown order (§7.3).
- `src/core/pd_audio_sink.h` / `pd_audio_sink_generator.*` — **retired** on
  desktop (M5); kept as the Android a1 fallback until M6, then removed.
- `CMakeLists.txt` — vendor PortAudio (top-level or in place); a
  `NATIVE_AUDIO` option (default ON); per-platform link (CoreAudio/ALSA);
  M6: an `AUDIO_AAUDIO` option that adds the aaudio hostapi + AAudio lib on
  Android; the a1 fallback compile flag for pre-M6 Android.

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

- **Worker refactor size** — the biggest change is stripping DSP/pacing from
  `libpd_worker::run()` and adding the shared audio thread + registry. The
  v1 worker is well-understood and the callback pattern is validated by
  `pdtest_portaudio.c`; the spike (§9) should shrink this risk before we
  cut the worker loose.
- **openfile glitch (§7.2)** — the default (brief audio stall on load) is
  acceptable; the scratch-swap alternative is more code. Decide in the plan
  after seeing how often loads happen in the real app.
- **Output-hook hardening (§4.2)** — hooks now fire on a real-time thread;
  the MIDI output queue `push` must be verified non-blocking/non-allocating
  (covered by host test 1.4). This is the subtlest correctness item.
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

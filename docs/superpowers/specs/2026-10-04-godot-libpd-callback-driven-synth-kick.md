# Callback-Driven Synth Kick — Design

Supersedes the "self-paced synth worker" transport from the native-audio
spec (`2026-10-03-godot-libpd-native-audio-design.md`). All other M5
decisions (mix-down-as-separate-instance, Option A main-thread creation,
ring bounds-safety, `set_mixer` count validation) are unchanged.

## 1. Motivation

The v1 native-audio transport runs each SYNTH instance on a dedicated
worker thread that paces itself against a **software clock**
(`sleep_until(next_tick)`), while the PortAudio callback that consumes the
rings runs on the **hardware clock**. The two clocks drift.

Measured (`RDIAG=1`, 8 synths, 20 s each):

|                      | `sleep_until` (baseline) | spin-to-deadline |
|----------------------|--------------------------|------------------|
| on-time (≤1 block)   | 93.5 %                   | 94.6 %           |
| stale >1–2 blocks    | 5.7 %                    | 4.5 %            |
| p99 lag              | 5.05 ms                  | 1.77 ms          |
| max lag              | 11.35 ms                 | 8.48 ms          |

Spin-to-deadline tightens each worker's *own* deadline (kills the tail) but
cannot stop the predicted `next_tick` from drifting against the audio
clock — a few percent of gathers still land >1 block stale, which is the
crackle. The fix is structural: **stop self-pacing entirely. The PortAudio
callback is the single clock and it drives the renders.**

## 2. Architecture

```
                 PortAudio callback thread  (hardware clock, one tick = stream_blocksize frames)
                 ┌─────────────────────────────────────────────────────────────────────────┐
   tick n        │  1. kick all N synth rings  (kick_seq_ = n)                             │
                 │  2. wait for all N rings to finish  (done_seq_ == n)                     │
                 │  3. gather latest block per ring  → mix_in_                             │
                 │  4. libpd_set_instance(mix) ; libpd_process_float(mix)  → mix_out_      │
                 │  5. copy mix_out_ → device buffer                                       │
                 └──────────────┬──────────────────────────────────────────────────────────┘
                                │ kick (kick_seq_)         ▲ done (done_seq_)
              ┌─────────────────┴──────────────────────────┼──────────────────────┐
              ▼                                            │                      ▼
   SYNTH worker 0 (own thread, pd_this = inst0)   SYNTH worker 1         SYNTH worker N-1
   ┌───────────────────────────────┐   ┌───────────────────────────────┐
   │ wait for kick_seq_ > done_seq_│   │  (same loop)                  │
   │ render 1 block                │   │                               │
   │ push block to its ring        │   │                               │
   │ done_seq_ = kick_seq_         │   │                               │
   └───────────────────────────────┘   └───────────────────────────────┘
```

Key invariants:

- **One clock.** The hardware-clocked callback launches every synth render.
  There is no software clock and no `sleep_until` in the synth path. The only
  residual timing error is dispatch→complete latency *inside* one tick
  (microseconds), not cross-tick drift.
- **Each SYNTH still owns its own thread.** libpd/pd calls for instance *i*
  happen only on worker *i* (per-thread `pd_this`, per-instance `i_mutex`) —
  the same invariant as the a1 model. The callback never renders a synth
  instance itself; it only *kicks* the workers.
- **Mix-down unchanged.** Created on the main thread (Option A), rendered on
  the callback thread under `mix_render_lock_`, `libpd_set_instance(mix)`
  once per callback. The callback is already the mix's render thread.
- **The rings keep their bounds-safety.** The gather path is untouched; only
  the *producer* side changes from "push on a self clock" to "push when
  kicked."

## 3. Kick sequencer (per `MixInputRing`)

Add to `MixInputRing` (it is the object both the callback and the owning
worker already hold):

```cpp
// Kick/done sequencer (callback-driven transport).
std::atomic<int> kick_seq_{0};   // incremented by the callback to request a render
std::atomic<int> done_seq_{0};   // written by the owning worker after it pushes

// callback: request one render
void kick() { kick_seq_.fetch_add(1, std::memory_order_release); }

// callback: block until the owning worker has completed kick #p_target.
void wait_done(int p_target) const {
    while (done_seq_.load(std::memory_order_acquire) < p_target) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}
```

The sequencer is a plain atomic counter pair — no mutex, no condition
variable. The worker polls `kick_seq_` (bounded sleep when idle); the
callback polls `done_seq_` (bounded sleep while waiting). Both spins are
short (µs) and bounded by the 5.8 ms tick budget.

## 4. SYNTH worker loop (replaces the self-paced pacing)

In `LibpdWorker::run()`, the SYNTH branch becomes "render when kicked":

```cpp
// SYNTH: no self-pacing. Render exactly when the callback kicks this ring.
if (config.worker_ring != nullptr) {
    const MixInputRing *ring = config.worker_ring;
    for (;;) {
        const int target = ring->kick_seq().load(std::memory_order_acquire);
        if (target <= ring->done_seq().load(std::memory_order_relaxed)) {
            // Idle until the next kick. SYNTH_IDLE_SLEEP_US (200) is the
            // confirmed first-cut value and the documented portable-device
            // tunable (§7).
            if (stop_requested) break;
            std::this_thread::sleep_for(std::chrono::microseconds(SYNTH_IDLE_SLEEP_US));
            continue;
        }
        // Render one block on this worker thread (pd_this is this instance).
        const int bs = libpd_blocksize();
        const int frames = bs * n_out;
        if ((int)out_buffer.size() >= frames) {
            libpd_process_float(1, nullptr, out_buffer.data());
            ring->push(out_buffer.data(), bs);
        }
        ring->done_seq().store(target, std::memory_order_release);
    }
}
```

Notes:

- The worker binds its own instance once at start (existing
  `libpd_set_instance` / adopt logic) — unchanged.
- Command draining (openfile/closefile/stop) still happens on this thread,
  exactly as today; the kick loop simply replaces the pacing.
- No `next_tick`, no `sleep_until`, no software clock.

## 5. Callback (`NativeAudio::render_block`)

The existing gather→mix→copy body stays; the kick+wait is inserted **before**
the gather, and only when a mixer is bound (so the control phase stays
lock-free and the unbound fast path is untouched):

```cpp
if (!mixer_bound_.load(std::memory_order_acquire)) {
    std::fill_n(p_dev_out, (size_t)p_frames * (size_t)n_out, 0.0f);
    return; // existing fast path: no mixer, silence, no lock
}
std::lock_guard<std::mutex> lk(mix_render_lock_);
if (mix_pd_ == nullptr) return;

// NEW: drive the synths from this (hardware) clock, then wait for them.
{
    std::lock_guard<std::mutex> rk(rings_mu_);
    const int target = ++tick_seq_;                 // monotonic tick
    for (MixInputRing *r : rings_) {
        r->kick();                                 // request one render
        r->wait_done(target);                       // block until it's pushed
    }
}
// ... existing gather_latest_n into mix_in_, then set_instance + process_float,
// ... then copy mix_out_ to the device buffer.  (unchanged)
```

`tick_seq_` is a new `int` member of `NativeAudio` (callback-thread-only, so
no atomics needed). The kick+wait runs under `mix_render_lock_` (already
held) so it cannot interleave with the MIXER worker's control-plane ops.

## 6. Real-time budget

Per tick the callback now does, in order: N `kick` atomics → N bounded
waits → N gathers → one mix `process_float` → copy. The worker render+push
measured **< 0.004 ms**; N=8 gives < 0.1 ms of actual DSP. The wait is
bounded by each worker waking from its 200 µs idle sleep plus its < 0.004 ms
render — worst case ≈ 0.2 ms per worker, overlapping (workers render in
parallel), so total ≈ 0.2–0.3 ms. Against a 5.8 ms tick (256-frame block)
that is < 6 % of the budget, and it is *deterministic* (no drift) instead of
the previous unbounded cross-tick staleness.

## 7. Trade-offs and guards

- **Callback now blocks on workers.** This is the intended synchronous
  fan-out/fan-in.
  - **Decision (confirmed): plain unbounded wait first.** The first cut uses
    a plain `wait_done` with no deadline — simplest, and §6 says we are <6 %
    of the tick. A pathological worker (OS preemption > tick) would stall the
    audio thread for one tick; that is the same class of event that
    previously produced a >1-block stale ring, now visible *in that tick*
    instead of accumulated.
  - **Guard (deferred):** if a target misbehaves, add a per-worker deadline
    in `wait_done` that falls back to gathering the last pushed block (the
    ring still holds it). Not in the first cut.
- **Worker idle sleep — 200 µs, confirmed.** This bounds the wakeup latency
  the callback waits on.
  - **Decision (confirmed): keep 200 µs for the first cut.** It is already
    inside budget with margin on desktop.
  - **Documented tunable for portable devices (Knulli/Batocera):** if the tail
    or CPU cost matters on a low-power target, this value is the first knob —
    shorten it, or replace the bounded sleep with a futex/eventfd shim so the
    worker wakes on the kick with no polling window. Expose it as a constant
    (`SYNTH_IDLE_SLEEP_US`) so it is a one-line change per target.
- **No change to the a1 / Android path.** The SYNTH kick loop is taken only
  when `config.worker_ring != nullptr`; the `sink`-based a1/ANDROID roles keep
  their existing self-paced loop.

## 8. Migration plan

1. Add the kick/done sequencer + `tick_seq_` to `MixInputRing` /
   `NativeAudio` (pure additions; existing behavior preserved until step 3).
2. Rework the SYNTH worker branch to the kick loop (§4).
3. Insert the kick+wait into `render_block` (§5).
4. Remove the now-dead self-paced pacing (`next_tick`/`sleep_until`) from the
   SYNTH path (keep it for the a1/ANDROID sink path).
5. Update `native_audio_mixdown_tests.cpp` / `server_audio_tests.cpp`: a
   fake callback must now kick the rings and assert the workers only render
   when kicked (no self-paced advance).
6. Verify with `RDIAG=1 NM_SYNTHS=8`: expect **>99 % on-time, 0 % stale**,
   then a 60 s soak and the 29 host binaries.

## 9. Explicit non-goals

- No resampling, no ring-depth change, no mix-down structural change.
- No per-core pinning / RT-priority in the first cut (the design allows it as
  a follow-up since the pool is ours); add only if the §6 budget is
  exceeded on a target.
- No replacement of `MixInputRing`; the ring stays as the producer→consumer
  handoff (drop-in, proven bounds-safety).

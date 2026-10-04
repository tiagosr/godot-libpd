#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_port.h"

struct _pdinstance; // libpd (multi-instance API, z_libpd.h)

namespace godot_libpd {

class MixInputRing; // core/mix_input_ring.h (per-worker handoff ring)

/**
 * Mixes per-worker output into the device stream — M5 Task 8 rework
 * (approved direction):
 *
 * - The mix-down libpd instance renders on a DEDICATED, real-time-paced
 *   mix thread (mix_render_loop), which is the ONLY thread that ever
 *   binds it (libpd_set_instance, exactly once, at thread start) and the
 *   ONLY thread that calls its libpd_process_float (invariant #1).
 * - The PortAudio callback (render_block) does NO libpd work and takes no
 *   lock: it atomically loads the stereo mix-output ring (the loaded
 *   shared_ptr copy keeps the ring alive for the duration of the gather
 *   even if the control thread unbinds mid-drain) and copies the latest
 *   rendered block into the device buffer — silence when no mixer is
 *   bound.
 * - Worker input rings (register_worker_ring) feed the mix thread's
 *   gather, not the callback. clear_mixer() stops + joins the mix render
 *   thread and unpublishes the output ring, so a freed mix instance can
 *   never be reached by any live thread (invariant #1, T4 review);
 *   with_mixer_lock() still serializes the mix thread's process_float
 *   with the MIXER worker's control-plane ops (openfile/closefile/init/
 *   teardown).
 *
 * open() installs render_block() into the AudioPort and starts the device
 * thread; the mix-down binds later via set_mixer(). close() joins the
 * device thread first, then stops the mix render thread.
 *
 * Layout contract (Option B, M5 Task 6): the callback receives the
 * stream blocksize (a multiple of libpd_blocksize(), 64 in this vendored
 * build) of interleaved float channels; each worker MixInputRing is
 * 2ch at libpd_blocksize() frames.
 */
class NativeAudio {
public:
	~NativeAudio();

	/**
	 * Installs render_block() as the AudioPort callback and starts the
	 * device thread. p_blocksize must be a positive multiple of 64
	 * (libpd_blocksize in this vendored build); the device input is 0
	 * channels (mix inputs come from the worker rings). No mix-down is
	 * bound yet: the callback renders silence until set_mixer().
	 */
	bool open(AudioPort *p_port, int p_mix_n_in, int p_mix_n_out, int p_blocksize, int p_samplerate);
	void close(); // joins the audio thread, stops the mix render thread, frees state

	bool is_open() const {
		return open_;
	}
	AudioPort *port() const {
		return port_;
	}
	int blocksize() const {
		return blocksize_;
	}
	int samplerate() const {
		return samplerate_;
	}
	/** True while a mix-down instance is bound (set_mixer() accepted). */
	bool has_mixer() const {
		return mixer_bound_.load(std::memory_order_acquire);
	}

	/**
	 * Bind the mix-down instance and start its dedicated render thread.
	 *
	 * The mix-down renders on that thread (NOT in the audio callback):
	 * it binds the instance exactly once (libpd_set_instance, on the
	 * thread, invariant #1) and paces one stream block per
	 * blocksize/samplerate seconds — gathering each worker ring's latest
	 * K = blocksize / ring_blocksize blocks, rendering under
	 * mix_render_lock_, and pushing the stereo result into the
	 * mix-output ring that the real-time callback drains.
	 *
	 * p_mix_n_in / p_mix_n_out must match the open-time layout (invariant
	 * #3, T4 review: mismatched counts would overflow the staging
	 * buffers). One binding at a time: returns false if already bound or
	 * not open.
	 */
	bool set_mixer(struct _pdinstance *p_mix_pd, int p_mix_n_in, int p_mix_n_out);

	/**
	 * Unbind the mix-down: stop + join the mix render thread, drop the
	 * binding, unpublish the output ring, and clear the worker rings.
	 *
	 * After the join NO thread references the mix instance anymore, so
	 * the caller may free it even while the audio thread is still live
	 * (M5 Task 6, invariant #1 from the T4 review); the callback
	 * renders silence afterwards. Safe to call when no mixer is bound
	 * (no-op).
	 */
	void clear_mixer();

	/**
	 * Run p_fn while holding mix_render_lock_ — the lock the mix render
	 * thread holds around the mix-down's libpd_process_float. The MIXER
	 * role worker uses this for its control-plane ops
	 * (libpd_openfile / closefile / init / teardown) so they never race
	 * a mix render. The real-time drain path takes NO lock.
	 */
	template <typename F>
	void with_mixer_lock(F &&p_fn) {
		std::lock_guard<std::mutex> lock(mix_render_lock_);
		std::forward<F>(p_fn)();
	}

	// Per-worker output rings (M5 Task 5): each synth worker pushes its
	// own 2ch blocks here; the mix render thread gathers the latest
	// K blocks per ring — ring i feeds mix input channels 2i, 2i+1 —
	// in registration order.

	/**
	 * Register a worker's output ring. Layout contract (invariant #4,
	 * T5 review): 2ch rings whose blocksize divides the stream blocksize —
	 * the mix render thread gathers blocksize()/ring_blocksize() blocks per
	 * ring. The first registered ring pins the mix's ring blocksize
	 * (single-size contract); a different blocksize is rejected, as are
	 * non-2ch rings and duplicates. A ring registered before open() is
	 * validated against the default blocksize (256); open() starts from
	 * a clean ring list.
	 */
	void register_worker_ring(MixInputRing *p_ring);
	void unregister_worker_ring(MixInputRing *p_ring);
	bool has_worker_ring(MixInputRing *p_ring) const;

private:
	void render_block(float *p_dev_in, float *p_dev_out, int p_frames); // AudioPort callback: renders the mix-down

	AudioPort *port_ = nullptr;

	// Layout fixed by open() (invariant #3, T4 review).
	int ring_blocksize_ = 0; // pinned by the first registered ring
	int open_mix_n_in_ = 0;
	int open_mix_n_out_ = 0;
	int blocksize_ = 0; // stream blocksize
	int samplerate_ = 0;
	bool open_ = false;

	mutable std::mutex rings_mu_;
	std::vector<MixInputRing *> rings_; // registration order (no dups)

	// Staging: one stream block of the mix-down's inputs / outputs, plus
	// the gather_latest_n(K) scratch (2ch, K * 64 * 2 = 2 * blocksize
	// floats). Single-thread access: the mix render thread owns all of
	// it; the real-time callback never reads it (Task 8 rework).
	std::vector<float> mix_in_;
	std::vector<float> mix_out_;
	std::vector<float> gather_scratch_;

	// Mix-down binding: written by the control thread (set_mixer before
	// the render thread starts — std::thread's start provides the
	// happens-before edge the render thread's read needs; nulled again
	// only after the render thread is joined).
	struct _pdinstance *mix_pd_ = nullptr;
	std::atomic<bool> mixer_bound_{false}; // set_mixer() accepted / clear_mixer() released

	// Set by close() BEFORE the audio port is stopped, so the real-time
	// callback's wait_done() can abort if the synth workers stop first (they
	// will not signal done once stopped, and an unbounded wait would hang the
	// audio thread — and hence the close() join — forever).
	std::atomic<bool> closing_{false};

	// Serializes the real-time callback's mix render (render_block) with the
	// MIXER worker's control-plane ops (with_mixer_lock) and with
	// clear_mixer()'s unbind, so a freed mix instance is never rendered.
	std::mutex mix_render_lock_;

	// Callback-driven synth kick (M5 T9): monotonic tick counter, incremented
	// by render_block under mix_render_lock_ (callback thread only, no atomics
	// needed). Each ring's kick_seq_ is set to this value to request a render.
	int tick_seq_ = 0;
};

} // namespace godot_libpd

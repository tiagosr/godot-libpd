#pragma once
/**
 * @file native_audio.h
 * @brief Mix-down render in the PortAudio callback (M5 Task 4).
 *
 * Architecture (the invariant that makes this safe — do NOT "fix" it away):
 *
 * Each libpd instance's `libpd_process_float` runs on exactly ONE thread,
 * and that thread called `libpd_set_instance(it)` exactly ONCE. `pd_this`
 * is thread-local; switching it between live instances corrupts pd state.
 *
 * - Worker threads (a1 model, M3/T3) each render their own LibpdInstance
 *   on their own pthread, pinning it with one set_instance. They push
 *   their 2-channel blocks into `MixInputRing`s.
 * - THIS class owns the PortAudio stream and its callback. The callback
 *   renders ONLY the mix-down instance: it binds the mix instance with a
 *   one-shot `set_instance` before its first block, gathers the latest
 *   block from each registered `MixInputRing` into the mix inputs
 *   (ring i -> channels 2i, 2i+1, in the interleaved per-frame layout
 *   libpd's process expects), runs `libpd_process_float`, and copies the
 *   mix's `mix_n_out_` channels to the device output.
 * - The control thread never RENDERS the mixer; only this callback does.
 *   `set_mixer()` is pure state; control-plane ops on the mixer (e.g.
 *   libpd_openfile) are serialized against the callback via
 *   `with_mixer_lock()`.
 * - Teardown caveat (verified in pd 0.56.5 multi-instance): a thread must
 *   have `libpd_set_instance(it)` bound before it calls
 *   `libpd_closefile` on `it`'s canvas — `libpd_free_instance` resets the
 *   CALLING thread's `pd_this` to the main instance, so a `closefile`
 *   issued after some other instance's `free_instance` would free the
 *   canvas in the wrong instance context ("couldn't unbind" + the
 *   owning instance's canvas list left dangling). Do teardown after
 *   `close()` (callback thread joined), binding the instance first.
 *
 * This class is device-agnostic: any `AudioPort` works (see null_port.h
 * for the device-free test harness).
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "audio_port.h"

// Forward decl of libpd's instance struct (m_pd.h declares it and does
// `#define t_pdinstance struct _pdinstance`). Written as `struct _pdinstance`
// here so this header is self-contained while staying identical to the
// z_libpd.h API spelling after macro expansion.
struct _pdinstance;

namespace godot_libpd {

class MixInputRing; // forward decl — header-only, defined in mix_input_ring.h

class NativeAudio {
public:
	NativeAudio();
	~NativeAudio();

	/**
	 * Open the stream on p_port and start its audio thread.
	 *
	 * p_mix_n_in / p_mix_n_out: channel counts for the MIX-DOWN instance
	 * that `set_mixer()` will bind (the device itself gets 0 inputs and
	 * `p_mix_n_out` outputs — the mix-down's inputs are the worker rings,
	 * not the audio device).
	 *
	 * Returns false (and leaves the port untouched) if p_port is null,
	 * we're already open, `p_blocksize <= 0`, `p_blocksize % 64 != 0`
	 * (libpd processes in 64-frame ticks), `p_samplerate <= 0`,
	 * `p_mix_n_in < 0`, `p_mix_n_out <= 0`, or `p_port->open(...)` fails.
	 */
	bool open(AudioPort *p_port, int p_mix_n_in = 16, int p_mix_n_out = 2,
			int p_blocksize = 256, int p_samplerate = 44100);

	/**
	 * Stop the audio thread (joins it), close the port, drop the mixer
	 * binding and all registered rings, and terminate PortAudio once.
	 * Safe to call when closed. `open()` may be called again afterwards
	 * (the one-shot set_instance guard is reset).
	 */
	void close();

	/**
	 * Bind the mix-down instance (created + initialized on the mixer's
	 * worker thread, e.g. the LibpdServer worker) for this callback to
	 * render.
	 *
	 * `set_mixer()` itself is plain state (atomics); the actual
	 * `libpd_set_instance(mix)` happens inside the callback before its
	 * first block (one-shot guard), because the callback thread must be
	 * the thread that renders the mix instance.
	 *
	 * Contract: call at most once per open() with the instance that will
	 * live until close(); while the audio thread is live, only the callback
	 * may call `libpd_set_instance(mix)` (any other thread switching pd_this
	 * to a live instance it doesn't render corrupts pd state). After
	 * close() has joined the audio thread, the owner may bind and free the
	 * instance — but must bind BEFORE `libpd_closefile` (see the teardown
	 * caveat in the file header). Rebinding requires close() + open() first.
	 */
	void set_mixer(struct _pdinstance *p_mix_pd, int p_mix_n_in, int p_mix_n_out);

	/**
	 * Run p_fn under `mix_render_lock_` — the lock the audio callback
	 * holds while calling `libpd_process_float` on the mix-down instance.
	 * Control-plane operations on the mixer (e.g. `libpd_openfile`) must
	 * use this to serialize with rendering.
	 */
	template <typename F>
	void with_mixer_lock(F &&p_fn) {
		std::lock_guard<std::mutex> lk(mix_render_lock_);
		p_fn();
	}

	/**
	 * Register a worker's 2ch ring for gathering (ring i -> mix input
	 * channels 2i, 2i+1, in registration order). Rejected (ignored): null
	 * pointers, rings whose blocksize differs from the stream's, and
	 * duplicates. The ring must stay alive while registered.
	 */
	void register_worker_ring(MixInputRing *p_ring);

	/**
	 * Remove a ring. Safe while the audio thread is mid-block: the
	 * callback works from an in-block snapshot taken under the rings
	 * lock, and the ring object is owned by the caller (worker thread).
	 */
	void unregister_worker_ring(MixInputRing *p_ring);

	bool is_open() const { return open_; }
	AudioPort *port() const { return port_; }
	int blocksize() const { return blocksize_; }
	int samplerate() const { return samplerate_; }

private:
	// The callback body. Installed as the port's render callback in open()
	// (the port invokes it on the audio thread); never called directly.
	void render_block(float *p_dev_in, float *p_dev_out, int p_frames);
	// The mix-down instance to render. BOUND on the callback thread only:
	// set_mixer() (control thread) publishes it, and the callback does the
	// one-shot set_instance before its first process_float. Atomics because
	// it is written by the control thread and read by the audio thread.
	std::atomic<struct _pdinstance *> mix_pd_{nullptr};
	std::atomic<int> mix_n_in_{16};
	std::atomic<int> mix_n_out_{2};
	std::atomic<bool> mix_set_{false}; // one-shot: set_instance happened
	std::mutex mix_render_lock_;      // held around the mix process_float

	// Fixed-size staging buffers, sized at open() — no allocation in the
	// render path (real-time safety).
	std::vector<float> mix_in_;
	std::vector<float> mix_out_;

	// Per-ring scratch for the gather: gather_latest() returns one
	// interleaved 2ch block (2*blocksize floats), which render_block then
	// scatters into the two channel columns of mix_in_ (libpd reads its
	// inBuffer interleaved per frame: inBuffer[f * n_in + c]).
	std::vector<float> gather_scratch_;

	// Worker rings to gather (registration order). Written under rings_mu_
	// (control thread) and snapshotted under the same lock per block
	// (audio thread).
	std::vector<MixInputRing *> rings_;
	std::mutex rings_mu_;

	AudioPort *port_ = nullptr; // NOT owned
	int blocksize_ = 256;
	int samplerate_ = 44100;
	bool open_ = false;
};

} // namespace godot_libpd

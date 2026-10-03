#include "native_audio.h"

#include <algorithm>

#include "mix_input_ring.h"
#include "null_port.h" // for the PortAudio-terminate flag (see close())

// libpd multi-instance API (z_libpd.h re-declares the t_pdinstance typedef).
extern "C" {
#include "z_libpd.h"
}

// PortAudio terminate-once flag, shared with null_port.h.
extern "C" {
#include "portaudio.h"
}
static std::once_flag pa_terminate_once_;

namespace godot_libpd {

static void terminate_portaudio() {
	std::call_once(pa_terminate_once_, []() {
		Pa_Terminate();
	});
}

NativeAudio::NativeAudio() {}

NativeAudio::~NativeAudio() {
	close();
}

bool NativeAudio::open(AudioPort *p_port, int p_mix_n_in, int p_mix_n_out,
		int p_blocksize, int p_samplerate) {
	if (open_ || p_port == nullptr) {
		return false;
	}
	if (p_mix_n_in < 0 || p_mix_n_out <= 0 || p_samplerate <= 0 ||
			p_blocksize <= 0 || p_blocksize % 64 != 0) {
		return false;
	}
	mix_n_in_.store(p_mix_n_in);
	mix_n_out_.store(p_mix_n_out);
	open_mix_n_in_ = p_mix_n_in;
	open_mix_n_out_ = p_mix_n_out;
	blocksize_ = p_blocksize;
	samplerate_ = p_samplerate;
	// Fixed-size staging, sized once — the render path must not allocate.
	mix_in_.assign((size_t)p_blocksize * (size_t)p_mix_n_in, 0.0f);
	mix_out_.assign((size_t)p_blocksize * (size_t)p_mix_n_out, 0.0f);
	gather_scratch_.assign(2 * (size_t)p_blocksize, 0.0f);
	// Clean state: re-open after close() is legal; the one-shot set_instance
	// guard must start false.
	mix_pd_.store(nullptr);
	mix_set_.store(false);
	mixer_bound_.store(false);
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
	port_ = p_port;
	port_->set_render_callback([this](float *in, float *out, int frames) {
		this->render_block(in, out, frames);
	});
	// Device input count is 0: the mix-down's inputs are the worker rings
	// gathered in render_block, not the audio device.
	if (port_->open(0, p_mix_n_out, p_samplerate, p_blocksize) != 0) {
		port_ = nullptr;
		return false;
	}
	open_ = true;
	return true;
}

void NativeAudio::close() {
	if (!open_) {
		return;
	}
	// Joins the audio thread first: after this no render block can run.
	port_->close();
	open_ = false;
	// Never leave a stale mixer pointer around (the callback could still
	// hold the only reference into a freed instance).
	mix_pd_.store(nullptr);
	mix_set_.store(false);
	mixer_bound_.store(false);
	mix_n_in_.store(16);
	mix_n_out_.store(2);
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
	terminate_portaudio();
}

bool NativeAudio::set_mixer(struct _pdinstance *p_mix_pd, int p_mix_n_in, int p_mix_n_out) {
	// The counts must match the open-time layout: the staging buffers are
	// sized from open(), so a different mix shape would overflow
	// mix_in_/mix_out_ (invariant #3, T4 review).
	if (!open_ || p_mix_pd == nullptr ||
			p_mix_n_in != open_mix_n_in_ || p_mix_n_out != open_mix_n_out_) {
		return false;
	}
	// One binding at a time (M5 Task 6): rebind requires clear_mixer().
	if (mixer_bound_.exchange(true)) {
		return false;
	}
	// Plain state (atomics) — the actual libpd_set_instance(mix) happens
	// inside render_block before the first block, because the callback
	// thread must be the thread that binds AND renders the mix instance.
	// Channel counts are stored before the pointer, so a callback that
	// observes the new pointer also observes the new channel counts.
	mix_n_in_.store(p_mix_n_in);
	mix_n_out_.store(p_mix_n_out);
	mix_pd_.store(p_mix_pd);
	return true;
}

void NativeAudio::clear_mixer() {
	// Unbind under the render lock (invariant #1, T4 review): a callback
	// block that loaded the pointer before the clear holds
	// mix_render_lock_ and finishes that block before this returns;
	// every later callback loads nullptr and renders silence. The caller
	// may free the instance once this returns, even while the audio
	// thread is live — the stop path calls this BEFORE mixer teardown.
	std::lock_guard<std::mutex> lk(mix_render_lock_);
	mix_pd_.store(nullptr);
	// Reset the one-shot so a LATER rebind (possibly another instance)
	// re-binds on the callback thread.
	mix_set_.store(false);
	mixer_bound_.store(false);
	// Restore the open-time counts: render_block reads them for staging
	// even before the mixer check (mix_in_ is sized from open()).
	mix_n_in_.store(open_mix_n_in_);
	mix_n_out_.store(open_mix_n_out_);
}

void NativeAudio::register_worker_ring(MixInputRing *p_ring) {
	// Layout contract (M5 Task 6, invariant #4): 2ch rings whose blocksize
	// divides the stream blocksize — the callback gathers
	// blocksize()/ring_blocksize() blocks per ring (production case: a
	// libpd_blocksize() ring under a 256-frame stream). The first
	// registered ring fixes the mix's ring blocksize (single-size
	// contract); a different blocksize is rejected. A ring registered
	// before open() is validated against the default blocksize (256).
	if (p_ring == nullptr || p_ring->channels() != 2) {
		return;
	}
	std::lock_guard<std::mutex> lk(rings_mu_);
	const int stream = open_ ? blocksize_ : 256;
	if (stream % p_ring->blocksize() != 0) {
		return;
	}
	if (ring_blocksize_ != 0 && ring_blocksize_ != p_ring->blocksize()) {
		return;
	}
	for (MixInputRing *r : rings_) {
		if (r == p_ring) {
			return; // duplicate
		}
	}
	if (ring_blocksize_ == 0) {
		ring_blocksize_ = p_ring->blocksize();
	}
	rings_.push_back(p_ring);
}

bool NativeAudio::has_worker_ring(MixInputRing *p_ring) const {
	std::lock_guard<std::mutex> lk(rings_mu_);
	for (MixInputRing *r : rings_) {
		if (r == p_ring) {
			return true;
		}
	}
	return false;
}

void NativeAudio::unregister_worker_ring(MixInputRing *p_ring) {
	// Safe mid-block: the callback works from a snapshot taken under
	// rings_mu_ (the whole gather holds the lock), so a concurrent
	// unregister waits for the in-block gather to finish; the ring object
	// itself is owned by the caller.
	std::lock_guard<std::mutex> lk(rings_mu_);
	rings_.erase(std::remove(rings_.begin(), rings_.end(), p_ring), rings_.end());
	if (rings_.empty()) {
		// No registered rings: the mix's ring blocksize is unset again, so
		// the next registration re-establishes it (single-size contract
		// applies to the concurrently registered rings).
		ring_blocksize_ = 0;
	}
}

void NativeAudio::render_block(float *p_dev_in, float *p_dev_out, int p_frames) {
	// The port contract is a fixed blocksize; clamp defensively so a
	// misbehaving port can never overflow the fixed-size staging buffers.
	if (p_frames > blocksize_) {
		p_frames = blocksize_;
	}
	// 1. Gather each worker ring's latest K blocks into mix_in_ (ring i ->
	//    channels 2i, 2i+1), K = p_frames / ring_blocksize (M5 Task 6,
	//    Option B: the rings are pushed at the libpd blocksize, so one
	//    callback consumes K of them). An empty ring gathers silence (0.0)
	//    — a worker not yet started contributes zeros, not stale audio.
	//    The gather holds rings_mu_ so an unregister cannot race it.
	//
	//    libpd reads its inBuffer INTERLEAVED per frame
	//    (inBuffer[f * n_in + c]; the PROCESS macro in z_libpd.c), while
	//    gather_latest_n() yields K interleaved stereo blocks per ring — so
	//    each ring's samples are SCATTERED into its two channel columns,
	//    never copied contiguously (a contiguous block would smear the
	//    ring's channels across every mix input channel).
	{
		const int n_in = mix_n_in_.load();
		std::lock_guard<std::mutex> lk(rings_mu_);
		// Zero first: channels not covered by a registered ring (e.g. a
		// just-unregistered ring) must contribute silence, not stale audio.
		std::fill_n(mix_in_.data(), (size_t)p_frames * (size_t)n_in, 0.0f);
		const int k = ring_blocksize_ > 0 ? p_frames / ring_blocksize_ : 0;
		int i = 0;
		for (MixInputRing *r : rings_) {
			if (2 * i + 1 >= n_in) {
				break; // defensive: more rings than mix inputs — extras stay silent
			}
			if (k > 0) {
				// gather_scratch_ holds 2*blocksize_ floats >= K * 2ch * ring
				// blocksize; gather_latest_n() always writes the full window
				// (missing older blocks are silence).
				r->gather_latest_n(gather_scratch_.data(), k);
				const float *s = gather_scratch_.data();
				const size_t col_l = 2 * (size_t)i;
				const size_t col_r = col_l + 1;
				for (int f = 0; f < p_frames; ++f) {
					mix_in_[(size_t)f * n_in + col_l] = s[2 * f];
					mix_in_[(size_t)f * n_in + col_r] = s[2 * f + 1];
				}
			}
			++i;
		}
	}
	// 2. Acquire the render lock BEFORE loading the mixer pointer
	//    (invariant #1, M5 Task 6): clear_mixer() unbinds under the same
	//    lock, so a callback that loaded a live pointer holds the lock
	//    until its process_float finishes — a concurrent clear (and any
	//    subsequent free of the instance) cannot interleave.
	std::lock_guard<std::mutex> lk(mix_render_lock_);
	struct _pdinstance *mix = mix_pd_.load(std::memory_order_acquire);
	if (mix == nullptr) {
		// No mixer bound (or cleared): contribute silence.
		std::fill_n(p_dev_out, (size_t)p_frames * (size_t)mix_n_out_.load(), 0.0f);
		return;
	}
	// 3. One-shot: bind the mix-down on THIS callback thread before its
	//    first process_float. No other thread ever calls
	//    libpd_set_instance(mix) — that is the whole invariant.
	if (!mix_set_.exchange(true)) {
		libpd_set_instance(mix);
	}
	// 4. (mix_render_lock_ is already held — see step 2.) Serialize with
	//    control-plane ops on the mixer (with_mixer_lock).
	const int ticks = p_frames / 64; // libpd_blocksize() is 64
	libpd_process_float(ticks, mix_in_.data(), mix_out_.data());
	// 5. Copy the mix's stereo output to the device output.
	std::copy_n(mix_out_.data(), (size_t)p_frames * (size_t)mix_n_out_.load(), p_dev_out);
	(void)p_dev_in; // device inputs unused: mix inputs come from the rings
}

} // namespace godot_libpd

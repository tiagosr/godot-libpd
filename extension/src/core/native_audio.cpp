#include "native_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>

#include "mix_input_ring.h"

// libpd multi-instance API (z_libpd.h re-declares the t_pdinstance typedef).
extern "C" {
#include "z_libpd.h"
}

using namespace godot_libpd;

NativeAudio::~NativeAudio() {
	close();
}

bool NativeAudio::open(AudioPort *p_port, int p_mix_n_in, int p_mix_n_out, int p_blocksize,
		int p_samplerate) {
	// A mix stream blocksize must be a positive multiple of the libpd
	// blocksize (64 in this vendored build, libpd_blocksize()): the render
	// path feeds it whole blocks (stream_blocksize / 64 ticks).
	if (open_ || p_port == nullptr || p_mix_n_in <= 0 || p_mix_n_out <= 0 ||
			p_blocksize <= 0 || p_blocksize % 64 != 0 || p_samplerate <= 0) {
		return false;
	}
	port_ = p_port;
	open_mix_n_in_ = p_mix_n_in;
	open_mix_n_out_ = p_mix_n_out;
	blocksize_ = p_blocksize;
	samplerate_ = p_samplerate;
	closing_.store(false, std::memory_order_relaxed);
	mix_in_.resize((size_t)p_blocksize * (size_t)p_mix_n_in, 0.0f);
	mix_out_.resize((size_t)p_blocksize * (size_t)p_mix_n_out, 0.0f);
	// gather_latest_n(K) scratch: K = blocksize / ring_blocksize, each block
	// 2ch * ring_blocksize floats -> K * 2 * ring_blocksize = 2 * blocksize.
	gather_scratch_.resize(2 * (size_t)p_blocksize, 0.0f);
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
	// The device carries only OUTPUT channels (0 in): mix inputs come from
	// the per-worker rings, not the hardware (Task 4 rework). The real-time
	// drain callback is installed before the stream starts.
	port_->set_render_callback([this](float *in, float *out, int frames) {
		render_block(in, out, frames);
	});
	if (port_->open(0, p_mix_n_out, p_samplerate, p_blocksize) != 0) {
		port_ = nullptr;
		return false;
	}
	open_ = true;
	return true;
}

void NativeAudio::register_worker_ring(MixInputRing *p_ring) {
	// Layout contract (M5 Task 6, invariant #4): 2ch rings whose blocksize
	// divides the stream blocksize — the mix render thread gathers
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
			// Re-registration (tracked pre-open rings replayed at open time):
			// refresh the per-kick window + capacity for the ACTUAL stream
			// blocksize, which may differ from the pre-open default.
			p_ring->set_mix_blocksize(stream);
			p_ring->ensure_capacity(stream / p_ring->blocksize());
			return;
		}
	}
	if (ring_blocksize_ == 0) {
		ring_blocksize_ = p_ring->blocksize();
	}
	// Tell the ring how many frames the callback renders per tick, so its
	// kick-driven SYNTH worker renders that many ring-blocks per kick (a
	// contiguous window) instead of one block. The ring must hold at least
	// that many blocks — a capped gather window leaves the older part of
	// each mix block silence/stale (warble + crackle; M6' Android 1024-
	// frame stream vs 8-block rings).
	p_ring->set_mix_blocksize(stream);
	p_ring->ensure_capacity(stream / p_ring->blocksize());
	rings_.push_back(p_ring);
}

void NativeAudio::unregister_worker_ring(MixInputRing *p_ring) {
	// Safe mid-block: the mix render thread works from a snapshot taken
	// under rings_mu_ (the whole gather holds the lock), so a concurrent
	// unregister waits for the in-block gather to finish; the ring object
	// itself is owned by the caller (a later unregister is a no-op once
	// the mix has cleared its list).
	std::lock_guard<std::mutex> lk(rings_mu_);
	rings_.erase(std::remove(rings_.begin(), rings_.end(), p_ring), rings_.end());
	if (rings_.empty()) {
		// No registered rings: the mix's ring blocksize is unset again, so
		// the next registration re-establishes it (single-size contract
		// applies to the concurrently registered rings).
		ring_blocksize_ = 0;
	}
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


void NativeAudio::render_block(float *p_dev_in, float *p_dev_out, int p_frames) {
	// The mix-down renders HERE (the mix-in-callback model): bind the mix
	// instance on this thread, gather each worker ring's latest K blocks
	// into mix_in_ (ring i -> channels 2i, 2i+1), render one stream block,
	// and copy the result to the device buffer. Silence when no mixer is
	// bound.
	//
	// T8 root-cause fix: the mix instance is created on the MAIN thread
	// (LibpdInstance::init for the MIXER role), NOT on the MIXER worker.
	// Cross-thread create->render (worker creates, this callback renders) was
	// the intermittent heap-corruption trigger; main-thread creation + this
	// callback's render is stable (see the mode-0 repro in spike/). Render the
	// mix under mix_render_lock_, which serializes with the MIXER worker's
	// control-plane ops (with_mixer_lock) and with clear_mixer() unbind, so a
	// freed mix instance is never rendered.
	if (p_frames <= 0) {
		return;
	}
	const int n_out = open_mix_n_out_;
	// Silence the whole device buffer first: p_frames can exceed the
	// rendered block when the host delivers a larger callback, and the
	// remainder must be zero, never stale device memory.
	std::fill_n(p_dev_out, (size_t)p_frames * (size_t)n_out, 0.0f);
	// Fast path (real-time-safe): no mixer bound -> silence, no lock. This
	// keeps the callback lock-free during the control phase (init/openfile,
	// before set_mixer). mixer_bound_ is set/cleared by the control thread;
	// a stale read can only skip a render (silence) and never races a free
	// (clear_mixer() nulls mix_pd_ under the same lock the slow path takes).
	if (!mixer_bound_.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard<std::mutex> lk(mix_render_lock_);
	if (mix_pd_ == nullptr) {
		return; // unbound mid-block: silence
	}
	// Callback-driven synth kick (M5 T9): the PortAudio callback is the single
	// clock. Drive every registered synth ring from this tick, then wait for
	// each owning worker to finish its block, so the gather below sees FRESH
	// blocks (no self-paced software-clock drift — the crackle source).
	// kick_seq_ is set to this tick for each ring; each worker renders one
	// block + pushes, then writes done_seq_ = this tick. Plain wait first; a
	// per-worker deadline guard is a documented later step.
	{
		std::lock_guard<std::mutex> rk(rings_mu_);
		const int target = ++tick_seq_;
		for (MixInputRing *r : rings_) {
			r->kick(target);
		}
		for (MixInputRing *r : rings_) {
			r->wait_done(target, closing_);
		}
	}
	const int n_in = open_mix_n_in_;
	const int ticks = blocksize_ / 64; // libpd_blocksize()
	// Gather each worker ring's latest K blocks into mix_in_ (ring i ->
	// channels 2i, 2i+1); K = blocksize_ / ring_blocksize_. libpd reads
	// inBuffer interleaved per frame, so each ring's samples are SCATTERED
	// into its two channel columns, never copied contiguously.
	{
		std::lock_guard<std::mutex> rk(rings_mu_);
		std::fill_n(mix_in_.data(), mix_in_.size(), 0.0f);
		const int k = ring_blocksize_ > 0 ? blocksize_ / ring_blocksize_ : 0;
		int i = 0;
		for (MixInputRing *r : rings_) {
			if (2 * i + 1 >= n_in) {
				break;
			}
			if (k > 0) {
				r->gather_latest_n(gather_scratch_.data(), k);
				const float *s = gather_scratch_.data();
				const size_t col_l = 2 * (size_t)i;
				for (int f = 0; f < blocksize_; ++f) {
					mix_in_[(size_t)f * (size_t)n_in + col_l] = s[2 * f];
					mix_in_[(size_t)f * (size_t)n_in + col_l + 1] = s[2 * f + 1];
				}
			}
			++i;
		}
		// DIAGNOSTIC (RDIAG=1): report how stale each ring's newest block is.
		// A healthy producer reads well under one libpd block period; a large or
		// erratic lag means the synth worker is missing its submission deadline.
		static int rdiag_count = 0;
		if (std::getenv("RDIAG") != nullptr && ((rdiag_count++) % 20) == 0) {
			char line[256];
			int off = std::snprintf(line, sizeof(line), "[rdiag] lag_ms: ");
			int n = 0;
			for (MixInputRing *r : rings_) {
				const double lag = r->lag_ms();
				off += std::snprintf(line + off, sizeof(line) - (size_t)off, "r%d=%.2f ", n, lag);
				++n;
			}
			std::fwrite(line, 1, (size_t)off, stderr);
			std::fflush(stderr);
		}
	}
	// Bind + render the mix-down on this thread.
	libpd_set_instance(mix_pd_);
	libpd_process_float(ticks, mix_in_.data(), mix_out_.data());
	// Copy the rendered block (blocksize_ frames) to the device buffer;
	// the remainder (if p_frames > blocksize_) is the silence fill above.
	const int frames = std::min(p_frames, blocksize_);
	for (int f = 0; f < frames; ++f) {
		for (int c = 0; c < n_out; ++c) {
			p_dev_out[f * (size_t)n_out + c] = mix_out_[(size_t)f * (size_t)n_out + c];
		}
	}
	(void)p_dev_in; // device inputs unused: mix inputs come from the worker rings
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
	mix_pd_ = p_mix_pd;
	// The real-time callback renders the mix directly (mix-in-callback
	// model); there is no dedicated render thread.
	return true;
}

void NativeAudio::clear_mixer() {
	if (!mixer_bound_.exchange(false)) {
		return;
	}
	// Unbind under mix_render_lock_: waits for any in-flight callback render
	// to finish, so the caller may free the mix instance without racing the
	// callback's render. After unbind the callback renders silence.
	{
		std::lock_guard<std::mutex> lk(mix_render_lock_);
		mix_pd_ = nullptr;
	}
	// Drop the worker rings (same drop as close()): a worker that
	// unregisters later finds nothing to remove, and its own ring object
	// stays owned (and alive) by the worker.
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
}

void NativeAudio::close() {
	if (!open_) {
		return;
	}
	// Abort any in-flight wait_done() in the real-time callback BEFORE joining
	// the audio thread: once the synth workers are stopped they will not signal
	// done, so an unbounded wait would hang the audio thread (and this join)
	// forever.
	closing_.store(true, std::memory_order_release);
	// Joins the audio thread first: after this no callback block can run,
	// so unbinding + clearing is safe (no concurrent render). The mix-down
	// instance is owned by the caller (the MIXER worker in production, the
	// harness in host tests) — never freed here.
	port_->close();
	open_ = false;
	mixer_bound_.store(false);
	mix_pd_ = nullptr;
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
}

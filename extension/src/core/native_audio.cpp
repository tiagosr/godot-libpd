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

// PortAudio terminate-once flag, shared with null_port.h.
extern "C" {
#include "portaudio.h"
}

using namespace godot_libpd;

// PortAudio's Pa_Terminate() is global and one-shot; close() may run from
// several NativeAudio instances (and tests), so guard it.
static std::once_flag pa_terminate_once_;

static void terminate_portaudio() {
	std::call_once(pa_terminate_once_, []() {
		Pa_Terminate();
	});
}

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
			return; // duplicate
		}
	}
	if (ring_blocksize_ == 0) {
		ring_blocksize_ = p_ring->blocksize();
	}
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

void NativeAudio::mix_render_loop() {
	// Invariant #1 (T4 review): bind ONCE on this thread, never switch,
	// never null mid-life — this thread is the only one that ever renders
	// the mix-down. mix_pd_ was published by set_mixer() before this
	// thread started (std::thread's start is the happens-before edge).
	libpd_set_instance(mix_pd_);
	const int n_in = open_mix_n_in_;
	const int ticks = blocksize_ / 64; // libpd_blocksize()
	const int sleep_us = blocksize_ * 1000000 / samplerate_;
	while (mix_running_.load(std::memory_order_relaxed)) {
		// 1. Gather each worker ring's latest K blocks into mix_in_ (ring i
		//    -> channels 2i, 2i+1), K = blocksize_ / ring_blocksize_ — the
		//    same gather the old callback used. An empty ring gathers
		//    silence; a just-unregistered ring contributes nothing. The
		//    gather holds rings_mu_ only so an unregister cannot race it.
		//
		//    libpd reads its inBuffer INTERLEAVED per frame
		//    (inBuffer[f * n_in + c]); gather_latest_n() yields K
		//    interleaved 2ch blocks per ring — so each ring's samples are
		//    SCATTERED into its two channel columns, never copied
		//    contiguously.
		{
			std::lock_guard<std::mutex> lk(rings_mu_);
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
		}
		// 2. Render the mix-down under mix_render_lock_ — the MIXER
		//    worker's control-plane ops (openfile/closefile/init/teardown)
		//    and any with_mixer_lock() hold this lock, so none of them can
		//    race this process_float.
		{
			std::lock_guard<std::mutex> lk(mix_render_lock_);
			libpd_process_float(ticks, mix_in_.data(), mix_out_.data());
		}
		// 3. Publish the rendered block to the real-time drain path (the
		//    ring's own internal mutex: one short copy, no libpd). The
		//    ring is guaranteed non-null while this thread runs:
		//    set_mixer() publishes it before the start, and
		//    clear_mixer()/close() join us before dropping it.
		mix_out_ring_->push(mix_out_.data(), blocksize_);
		// 4. Pace to real time: one stream block per blocksize_/samplerate_.
		std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
	}
}

void NativeAudio::render_block(float *p_dev_in, float *p_dev_out, int p_frames) {
	// Real-time-safe: NO libpd, NO lock, NO allocation (Task 8 rework).
	// This callback only DRAINS the stereo mix-output ring that the
	// dedicated mix render thread fills: it atomically loads a shared_ptr
	// copy of the ring (the copy keeps the ring alive for the gather even
	// if the control thread unbinds mid-drain) and copies the latest
	// rendered block into the device buffer — silence when no mixer is
	// bound.
	if (p_frames <= 0) {
		return;
	}
	const int n_out = open_mix_n_out_;
	// Silence the WHOLE device buffer first: the gathered block may be
	// shorter than p_frames when the host delivers a callback whose frame
	// count differs from the ring's blocksize, and the remainder must be
	// zero, never stale device memory.
	std::fill_n(p_dev_out, (size_t)p_frames * (size_t)n_out, 0.0f);
	const int frames = std::min(p_frames, blocksize_);
	const std::shared_ptr<MixInputRing> out = std::atomic_load(&mix_out_ring_);
	if (out != nullptr) {
		// gather_latest() copies at most min(frames, blocksize) * n_out
		// floats — never past the device buffer.
		out->gather_latest(p_dev_out, frames);
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
	// Publish the output ring BEFORE starting the render thread: the
	// real-time path then always sees either the previous (silenced)
	// ring or this one, and the render thread can push immediately.
	std::atomic_store(&mix_out_ring_,
			std::make_shared<MixInputRing>(open_mix_n_out_, blocksize_, 8));
	mix_running_.store(true, std::memory_order_release);
	mix_render_thread_ = std::thread(&NativeAudio::mix_render_loop, this);
	return true;
}

void NativeAudio::clear_mixer() {
	if (!mixer_bound_.exchange(false)) {
		return;
	}
	// Stop + join the mix render thread (invariant #1): after the join NO
	// thread references the mix-down instance anymore, so the caller may
	// free it even while the audio thread is still live (M5 Task 6,
	// invariant #1 from the T4 review). The stop path calls this BEFORE
	// tearing down the mixer instance.
	mix_running_.store(false, std::memory_order_release);
	if (mix_render_thread_.joinable()) {
		mix_render_thread_.join();
	}
	mix_pd_ = nullptr;
	// Unpublish the output ring: a real-time block that already loaded the
	// old shared_ptr keeps the ring alive through its final drain; every
	// later block loads nullptr and renders silence. No lock, no wait.
	std::atomic_store(&mix_out_ring_, std::shared_ptr<MixInputRing>());
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
	// Joins the audio thread first: after this no drain block can run.
	port_->close();
	open_ = false;
	// Stop the mix render thread and drop the binding (same teardown as
	// clear_mixer()). The mix-down instance is owned by the caller (the
	// MIXER worker in production, the harness in host tests) — never
	// freed here (M5 Task 8 rework).
	mix_running_.store(false, std::memory_order_release);
	if (mix_render_thread_.joinable()) {
		mix_render_thread_.join();
	}
	mixer_bound_.store(false);
	mix_pd_ = nullptr;
	// The audio thread is joined: a plain store cannot race the callback.
	mix_out_ring_ = nullptr;
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
		ring_blocksize_ = 0;
	}
	terminate_portaudio();
}

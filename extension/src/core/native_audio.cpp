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
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
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
	mix_n_in_.store(16);
	mix_n_out_.store(2);
	{
		std::lock_guard<std::mutex> lk(rings_mu_);
		rings_.clear();
	}
	terminate_portaudio();
}

void NativeAudio::set_mixer(struct _pdinstance *p_mix_pd, int p_mix_n_in, int p_mix_n_out) {
	// Plain state (atomics) — the actual libpd_set_instance(mix) happens
	// inside render_block before the first block, because the callback
	// thread must be the thread that binds AND renders the mix instance.
	// Channel counts are stored before the pointer, so a callback that
	// observes the new pointer also observes the new channel counts.
	if (p_mix_n_in < 0 || p_mix_n_out <= 0) {
		return;
	}
	mix_n_in_.store(p_mix_n_in);
	mix_n_out_.store(p_mix_n_out);
	mix_pd_.store(p_mix_pd);
}

void NativeAudio::register_worker_ring(MixInputRing *p_ring) {
	// Layout contract: 2ch rings at the stream blocksize (ring i -> mix
	// channels 2i, 2i+1). A ring registered before open() is validated
	// against the default blocksize (256).
	if (p_ring == nullptr || p_ring->blocksize() != blocksize_ || p_ring->channels() != 2) {
		return;
	}
	std::lock_guard<std::mutex> lk(rings_mu_);
	for (MixInputRing *r : rings_) {
		if (r == p_ring) {
			return; // duplicate
		}
	}
	rings_.push_back(p_ring);
}

void NativeAudio::unregister_worker_ring(MixInputRing *p_ring) {
	// Safe mid-block: the callback works from a snapshot taken under
	// rings_mu_ (the whole gather holds the lock), so a concurrent
	// unregister waits for the in-block gather to finish; the ring object
	// itself is owned by the caller.
	std::lock_guard<std::mutex> lk(rings_mu_);
	rings_.erase(std::remove(rings_.begin(), rings_.end(), p_ring), rings_.end());
}

void NativeAudio::render_block(float *p_dev_in, float *p_dev_out, int p_frames) {
	// The port contract is a fixed blocksize; clamp defensively so a
	// misbehaving port can never overflow the fixed-size staging buffers.
	if (p_frames > blocksize_) {
		p_frames = blocksize_;
	}
	const int n_out = mix_n_out_.load();
	// 1. Gather each worker ring's latest block into mix_in_ (ring i ->
	//    channels 2i, 2i+1). An empty ring gathers silence (0.0) — a
	//    worker not yet started contributes zeros, not stale audio. The
	//    gather holds rings_mu_ so an unregister cannot race it.
	//
	//    libpd reads its inBuffer INTERLEAVED per frame
	//    (inBuffer[f * n_in + c]; the PROCESS macro in z_libpd.c), while
	//    gather_latest() yields one interleaved stereo block per ring — so
	//    each ring's samples are SCATTERED into its two channel columns,
	//    never copied contiguously (a contiguous block would smear the
	//    ring's channels across every mix input channel).
	{
		const int n_in = mix_n_in_.load();
		std::lock_guard<std::mutex> lk(rings_mu_);
		// Zero first: channels not covered by a registered ring (e.g. a
		// just-unregistered ring) must contribute silence, not stale audio.
		std::fill_n(mix_in_.data(), (size_t)p_frames * (size_t)n_in, 0.0f);
		int i = 0;
		for (MixInputRing *r : rings_) {
			if (2 * i + 1 >= n_in) {
				break; // defensive: more rings than mix inputs — extras stay silent
			}
			r->gather_latest(gather_scratch_.data(), p_frames);
			const float *s = gather_scratch_.data();
			const size_t col_l = 2 * (size_t)i;
			const size_t col_r = col_l + 1;
			for (int f = 0; f < p_frames; ++f) {
				mix_in_[(size_t)f * n_in + col_l] = s[2 * f];
				mix_in_[(size_t)f * n_in + col_r] = s[2 * f + 1];
			}
			++i;
		}
	}
	// 2. No mixer bound yet (set_mixer not called): contribute silence.
	struct _pdinstance *mix = mix_pd_.load(std::memory_order_acquire);
	if (mix == nullptr) {
		std::fill_n(p_dev_out, (size_t)p_frames * (size_t)n_out, 0.0f);
		return;
	}
	// 3. One-shot: bind the mix-down on THIS callback thread before its
	//    first process_float. No other thread ever calls
	//    libpd_set_instance(mix) — that is the whole invariant.
	if (!mix_set_.exchange(true)) {
		libpd_set_instance(mix);
	}
	// 4. Serialize with control-plane ops on the mixer (with_mixer_lock).
	std::lock_guard<std::mutex> lk(mix_render_lock_);
	const int ticks = p_frames / 64; // libpd_blocksize() is 64
	libpd_process_float(ticks, mix_in_.data(), mix_out_.data());
	// 5. Copy the mix's stereo output to the device output.
	std::copy_n(mix_out_.data(), (size_t)p_frames * (size_t)n_out, p_dev_out);
	(void)p_dev_in; // device inputs unused: mix inputs come from the rings
}

} // namespace godot_libpd

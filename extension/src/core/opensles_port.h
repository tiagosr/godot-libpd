#pragma once

#include <vector>

#include "SLES/OpenSLES.h"
#include "SLES/OpenSLES_Android.h" // SLBufferItf + simple-buffer-queue types

#include "audio_port.h"

namespace godot_libpd {

/**
 * OpenSL ES-backed AudioPort (Android; M6').
 *
 * Output-only int16 stereo stream on the AndroidSimpleBufferQueue source
 * (the same shape Godot's own Android audio driver uses — user-verified
 * working on the RG DS where AAudio is silent). Two fixed-size buffers of
 * frames_per_buffer() frames; the buffer-queue callback is the audio thread:
 * it invokes the RenderFn with exactly frames_per_buffer() frames, converts
 * the rendered floats to int16, and re-enqueues the recycled buffer.
 *
 * Device facts (RG DS, M6' T2 spike):
 *  - 256-frame buffers deliver bursty (~26% of callbacks late); 1024-frame
 *    buffers are smooth. Hence the fixed 1024-frame size and the server-side
 *    blocksize round-up.
 *  - Android OpenSL ES has no float PCM format; int16 is the only supported
 *    sample format here.
 *  - Sample rate is set via the SL_SAMPLINGRATE_* constants; only 44100 and
 *    48000 are expressible (Godot's mix rates).
 */
class OpenSLESPort final : public AudioPort {
public:
	/// Fixed buffer-queue buffer size in frames (device-verified smooth).
	static int frames_per_buffer() {
		return 1024;
	}
	/// Number of buffers the buffer queue holds.
	static int buffer_count() {
		return 2;
	}

	int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) override;
	void close() override;
	bool is_open() const override;

	std::vector<AudioDeviceInfo> list_inputs() const override;
	std::vector<AudioDeviceInfo> list_outputs() const override;

	double output_latency_ms() const override;
	double input_latency_ms() const override {
		return 0.0;
	}
	bool supports_input() const override {
		return false;
	}

	/// Buffer-queue Enqueue failures since open (0 while healthy).
	int enqueue_failures() const {
		return enqueue_fails_;
	}

private:
	/// Audio-thread body: render one buffer, convert, re-enqueue.
	void on_buffer_consumed();

	// Mid-open failure teardown (subset of close(); open_ is still false).
	void destroy_player_and_engine() {
		if (player_ != nullptr) {
			(*player_)->Destroy(player_);
			player_ = nullptr;
			play_itf_ = nullptr;
			queue_itf_ = nullptr;
		}
		if (outmix_ != nullptr) {
			(*outmix_)->Destroy(outmix_);
			outmix_ = nullptr;
		}
		if (engine_ != nullptr) {
			(*engine_)->Destroy(engine_);
			engine_ = nullptr;
			engine_itf_ = nullptr;
		}
	}
	void destroy_engine_only() {
		if (engine_ != nullptr) {
			(*engine_)->Destroy(engine_);
			engine_ = nullptr;
			engine_itf_ = nullptr;
		}
	}

	static void SLAPIENTRY buffer_queue_trampoline(SLBufferQueueItf p_caller,
			void *p_context);

public:
	~OpenSLESPort() override;

private:
	bool open_ = false;
	SLObjectItf engine_ = nullptr;
	SLEngineItf engine_itf_ = nullptr;
	SLObjectItf outmix_ = nullptr;
	SLObjectItf player_ = nullptr;
	SLPlayItf play_itf_ = nullptr;
	SLBufferQueueItf queue_itf_ = nullptr;

	int samplerate_ = 0;
	int ring_ = 0; // next buffer slot to refill
	int enqueue_fails_ = 0;

	// Persistent buffer-queue buffers: buffer_count() x
	// frames_per_buffer() * 2 int16 samples. The NDK buffer queue's
	// Enqueue() takes the raw data pointer (no SLBufferItf header).
	std::vector<short> pcm_; // [slot * frames * 2 + i]
	// Render scratch (caller-allocated, zero-initialized per contract).
	std::vector<float> scratch_;
};

} // namespace godot_libpd

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace godot_libpd {

/** Static info about one input or output audio device. */
struct AudioDeviceInfo {
	int index = -1;
	std::string name;
	int max_in = 0;
	int max_out = 0;
};

/**
 * Per-block render callback — the audio-thread body.
 * p_dev_in has `frames * n_ins` floats (nullptr when n_ins == 0);
 * p_dev_out is caller-allocated, zero-initialized scratch that the callback
 * must fill with `frames * n_out` floats.
 */
using RenderFn = std::function<void(float *p_dev_in, float *p_dev_out, int p_frames)>;

/**
 * Platform-free audio port: one device stream, device listing, latency
 * reporting. The per-block render callback is installed once (before
 * open()) and invoked by the backend on the audio thread.
 */
class AudioPort {
public:
	virtual ~AudioPort() = default;

	/** Opens the stream. Returns 0 on success, nonzero (port-specific) on failure. */
	virtual int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) = 0;

	/** Closes the stream. Idempotent. */
	virtual void close() = 0;

	virtual bool is_open() const = 0;

	virtual std::vector<AudioDeviceInfo> list_inputs() const = 0;
	virtual std::vector<AudioDeviceInfo> list_outputs() const = 0;

	/** Output latency in milliseconds (0 if not open). */
	virtual double output_latency_ms() const = 0;

	/** Input latency in milliseconds (0 if not open). */
	virtual double input_latency_ms() const = 0;

	virtual bool supports_input() const = 0;

	/** Installs the per-block render callback. See RenderFn for the contract. */
	void set_render_callback(RenderFn p_fn) {
		render_ = std::move(p_fn);
	}

protected:
	RenderFn render_;
};

/**
 * Sums p_n_inputs interleaved buffers (each p_frames * p_out_ch floats)
 * into p_out (p_frames * p_out_ch floats), clamping every sample to
 * [-1, 1]. With p_n_inputs == 0, writes 0.0 to every sample.
 */
inline void mix_block(float *p_out, int p_out_ch, const float *const *p_inputs, int p_n_inputs,
		int p_frames) {
	for (int f = 0; f < p_frames; f++) {
		for (int c = 0; c < p_out_ch; c++) {
			float acc = 0.0f;
			for (int i = 0; i < p_n_inputs; i++) {
				acc += p_inputs[i][f * p_out_ch + c];
			}
			if (acc > 1.0f) {
				acc = 1.0f;
			} else if (acc < -1.0f) {
				acc = -1.0f;
			}
			p_out[f * p_out_ch + c] = acc;
		}
	}
}

} // namespace godot_libpd

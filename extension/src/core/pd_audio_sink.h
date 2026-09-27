#pragma once

#include <atomic>

namespace godot_libpd {

/**
 * Audio sink for one libpd instance. Implemented by the worker thread's
 * output path; v1 ships DrySink (count only, for headless tests) and
 * GeneratorSink (Godot AudioStreamGenerator, Task 5). Native backends
 * (CoreAudio/OpenSL/ALSA) are a v2 swap-in behind this interface.
 */
class PdAudioSink {
public:
	virtual ~PdAudioSink() = default;

	/**
	 * Push one block of rendered audio.
	 * p_interleaved: frames * n_out floats, interleaved (pd process_float layout).
	 * p_frames: block size in sample frames (== libpd_blocksize()).
	 * p_n_out: output channel count.
	 * Called from the worker thread only.
	 */
	virtual void push_block(const float *p_interleaved, int p_frames, int p_n_out) = 0;

	/** Number of blocks pushed (atomic; readable from the main thread). */
	virtual uint64_t blocks_pushed() const = 0;
};

/**
 * Counts blocks without touching audio. Used by headless integration tests
 * and as the pre-sink default until GeneratorSink is wired (Task 5).
 */
class DrySink : public PdAudioSink {
public:
	void push_block(const float *p_interleaved, int p_frames, int p_n_out) override {
		count++;
		if (p_interleaved != nullptr) {
			float local = 0.0f;
			const int n = p_frames * p_n_out;
			for (int i = 0; i < n; i++) {
				const float a = p_interleaved[i] < 0.0f ? -p_interleaved[i] : p_interleaved[i];
				if (a > local) {
					local = a;
				}
			}
			const float cur = _peak.load();
			if (local > cur) {
				_peak.store(local);
			}
		}
	}

	uint64_t blocks_pushed() const override {
		return count;
	}

	/** Max absolute sample seen so far (0 if silent). Main-thread readable. */
	float peak() const {
		return _peak.load();
	}

	/** Reset the peak (e.g. between test phases). */
	void clear_peak() {
		_peak.store(0.0f);
	}

private:
	std::atomic<uint64_t> count{0};
	std::atomic<float> _peak{0.0f};
};

} // namespace godot_libpd

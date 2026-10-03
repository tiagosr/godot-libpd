#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "audio_port.h"

namespace godot_libpd {

/**
 * Device-free AudioPort that simulates an audio thread for host tests.
 *
 * While open, a background std::thread renders at real-time pace: each tick
 * it builds a zeroed dev_in (when n_ins > 0) and zeroed dev_out scratch,
 * calls the render callback, bumps frames_rendered(), records the max abs
 * sample of dev_out in last_out_peak(), and sleeps blocksize/samplerate
 * seconds. close() flips the open flag and joins the thread.
 *
 * Intended as the device-free driver for the full NativeAudio loop.
 */
class NullPort : public AudioPort {
public:
	~NullPort() override {
		close();
	}

	int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) override {
		if (p_blocksize <= 0 || p_blocksize % 64 != 0) {
			return -1;
		}
		n_ins_ = p_n_ins;
		n_out_ = p_n_out;
		samplerate_ = p_samplerate;
		blocksize_ = p_blocksize;
		frames_rendered_ = 0;
		last_out_peak_ = 0.0f;
		open_ = true;
		thread_ = std::thread([this]() {
			const int frames = blocksize_;
			const std::chrono::duration<double> tick((double)frames / (double)samplerate_);
			while (open_) {
				std::vector<float> dev_in;
				if (n_ins_ > 0) {
					dev_in.assign((size_t)n_ins_ * frames, 0.0f);
				}
				std::vector<float> dev_out((size_t)n_out_ * frames, 0.0f);
				render_(dev_in.empty() ? nullptr : dev_in.data(), dev_out.data(), frames);
				frames_rendered_++;
				float peak = 0.0f;
				for (float v : dev_out) {
					const float a = v < 0.0f ? -v : v;
					if (a > peak) {
						peak = a;
					}
				}
				last_out_peak_ = peak;
				std::this_thread::sleep_for(tick);
			}
		});
		return 0;
	}

	void close() override {
		if (!open_) {
			return;
		}
		open_ = false;
		thread_.join();
	}

	bool is_open() const override {
		return open_;
	}

	std::vector<AudioDeviceInfo> list_inputs() const override {
		return {{0, "Null In", 1, 0}};
	}

	std::vector<AudioDeviceInfo> list_outputs() const override {
		return {{0, "Null", 0, 2}};
	}

	double output_latency_ms() const override {
		return latency_ms();
	}

	double input_latency_ms() const override {
		return latency_ms();
	}

	bool supports_input() const override {
		return true;
	}

	/** Total frames rendered since open() (read any time; approximate while open). */
	uint64_t frames_rendered() const {
		return frames_rendered_.load();
	}

	/** Max abs sample observed in the most recent rendered block (0 before open). */
	float last_out_peak() const {
		return last_out_peak_.load();
	}

private:
	double latency_ms() const {
		if (!open_) {
			return 0.0;
		}
		return (double)blocksize_ / (double)samplerate_ * 1000.0;
	}

	std::atomic<bool> open_{false};
	std::atomic<uint64_t> frames_rendered_{0};
	std::atomic<float> last_out_peak_{0.0f};
	std::thread thread_;

	int n_ins_ = 0;
	int n_out_ = 0;
	int samplerate_ = 0;
	int blocksize_ = 0;
};

} // namespace godot_libpd

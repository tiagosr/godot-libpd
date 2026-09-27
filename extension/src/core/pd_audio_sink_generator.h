#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include "pd_audio_sink.h"

namespace godot_libpd {

/**
 * Godot-native sink for Godot 4.6's redesigned AudioStreamGenerator.
 *
 * Godot 4.6 moved the generator push API to AudioStreamGeneratorPlayback
 * (push_frame/push_buffer, stereo Vector2 frames) and removed
 * play_from_thread(). The playback ring buffer is consumed by the audio mix
 * thread and is not lock-free, so all AudioStreamGeneratorPlayback calls
 * must happen on the main thread.
 *
 * Therefore this sink splits the path in two:
 *   worker thread:  push_block() appends rendered blocks to a small
 *                   thread-safe handoff ring (drop-oldest when full).
 *   main thread:    pump() drains the handoff ring and pushes stereo frames
 *                   into the Godot playback buffer.
 */
class GeneratorSink : public PdAudioSink {
public:
	/// Configure from the main thread before the worker starts.
	void setup(int p_samplerate, int p_n_out);

	/// Bind the playback object (main thread). Null ref unbinds.
	void set_playback(const godot::Ref<godot::AudioStreamGeneratorPlayback> &p_playback);

	// PdAudioSink — worker thread.
	void push_block(const float *p_interleaved, int p_frames, int p_n_out) override;

	/// Drain the handoff ring into the Godot playback buffer (main thread).
	void pump();

	uint64_t blocks_pushed() const override;

	/** Max absolute sample rendered so far (0 if silent). Main-thread readable. */
	float peak() const {
		return _peak.load();
	}

private:
	std::mutex ring_mutex;
	std::deque<float> ring; // interleaved, handoff worker -> main
	size_t ring_frame_cap = 0;
	int n_out = 2;

	godot::Ref<godot::AudioStreamGeneratorPlayback> playback;

	std::atomic<uint64_t> count{0};
	std::atomic<float> _peak{0.0f};
};

} // namespace godot_libpd

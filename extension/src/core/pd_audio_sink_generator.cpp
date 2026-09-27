#include "core/pd_audio_sink_generator.h"

#include <algorithm>

#include <godot_cpp/variant/vector2.hpp>

namespace godot_libpd {

void GeneratorSink::setup(int p_samplerate, int p_n_out) {
	n_out = p_n_out > 0 ? p_n_out : 2;
	// ~250 ms of headroom: the worker renders in real time and the main
	// thread pumps once per frame, so the ring only absorbs jitter.
	ring_frame_cap = (size_t)p_samplerate / 4;
	if (ring_frame_cap < 1024) {
		ring_frame_cap = 1024;
	}
	ring.clear();
}

void GeneratorSink::set_playback(const godot::Ref<godot::AudioStreamGeneratorPlayback> &p_playback) {
	playback = p_playback;
}

void GeneratorSink::push_block(const float *p_interleaved, int p_frames, int p_n_out) {
	count++;

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

	std::lock_guard<std::mutex> lock(ring_mutex);
	size_t have = ring.size() / (size_t)n_out;
	while (have + (size_t)p_frames > ring_frame_cap && have > 0) {
		// Drop the oldest frame (interleaved: n_out floats).
		ring.pop_front();
		have--;
	}
	if (have + (size_t)p_frames > ring_frame_cap) {
		// Block larger than the whole ring: keep only the freshest tail.
		const size_t keep = ring_frame_cap * (size_t)n_out;
		const float *tail = p_interleaved + ((size_t)p_frames - ring_frame_cap) * (size_t)n_out;
		ring.assign(tail, tail + keep);
	} else {
		ring.insert(ring.end(), p_interleaved, p_interleaved + n);
	}
}

void GeneratorSink::pump() {
	if (playback.is_null()) {
		return;
	}
	const int avail = playback->get_frames_available();
	if (avail <= 0) {
		return;
	}

	// Drain as much as the Godot buffer can take.
	std::vector<float> chunk;
	{
		std::lock_guard<std::mutex> lock(ring_mutex);
		const int frames = std::min(avail, (int)(ring.size() / (size_t)n_out));
		if (frames <= 0) {
			return;
		}
		chunk.assign(ring.begin(), ring.begin() + frames * n_out);
		ring.erase(ring.begin(), ring.begin() + frames * n_out);
	}

	// Convert interleaved n_out to stereo Vector2 frames (4.6 push API).
	godot::PackedVector2Array out;
	out.resize((int)chunk.size() / n_out);
	for (size_t i = 0; i * (size_t)n_out < chunk.size(); i++) {
		const float l = chunk[i * (size_t)n_out];
		const float r = (n_out > 1) ? chunk[i * (size_t)n_out + 1] : l;
		out[(int)i] = godot::Vector2(l, r);
	}
	playback->push_buffer(out);
}

uint64_t GeneratorSink::blocks_pushed() const {
	return count;
}

} // namespace godot_libpd

#pragma once

#include <vector>

#include "portaudio.h"

#include "audio_port.h"

namespace godot_libpd {

/**
 * PortAudio-backed AudioPort (CoreAudio on macOS, ALSA on Linux).
 *
 * Enumerates devices, opens a float32 stream whose static C-callback
 * trampoline dispatches every block to the RenderFn installed via
 * set_render_callback(), and reports latencies from PaStreamInfo.
 * Pa_Initialize() runs exactly once per process.
 */
class PortAudioPort : public AudioPort {
public:
	PortAudioPort();

	~PortAudioPort() override;

	int open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) override;

	void close() override;

	bool is_open() const override;

	std::vector<AudioDeviceInfo> list_inputs() const override;
	std::vector<AudioDeviceInfo> list_outputs() const override;

	double output_latency_ms() const override;
	double input_latency_ms() const override;

	bool supports_input() const override;

private:
	// PortAudio C stream callback (PaStreamCallback signature: input
	// buffer first, output buffer second): thin passthrough of the raw
	// float32 device buffers to render_, then paContinue.
	static int pa_cb(const void *p_in, void *p_out, unsigned long p_frames,
			const PaStreamCallbackTimeInfo *p_time_info, PaStreamCallbackFlags p_status_flags,
			void *p_user_data);

	// Open PortAudio stream handle; nullptr when closed.
	PaStream *stream_ = nullptr;
	bool open_ = false;
	// Latencies in ms, from PaStreamInfo while the stream is open.
	double output_latency_ms_ = 0.0;
	double input_latency_ms_ = 0.0;
};

} // namespace godot_libpd

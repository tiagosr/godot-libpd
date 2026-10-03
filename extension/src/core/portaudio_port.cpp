#include "core/portaudio_port.h"

#include <mutex>

namespace godot_libpd {

namespace {

// Pa_Initialize() exactly once per process. Failures surface through the
// device queries that every entry point makes (Pa_GetDeviceCount() == 0,
// no default device).
std::once_flag g_pa_init_flag;

void ensure_portaudio_initialized() {
	std::call_once(g_pa_init_flag, []() {
		(void)Pa_Initialize();
	});
}

// Scans device d for capability and appends it to p_out if it has the
// side. Called from list_inputs()/list_outputs() with the wanted side.
void add_device_if_capable(std::vector<AudioDeviceInfo> &p_out, int p_d, bool p_want_input) {
	const PaDeviceInfo *info = Pa_GetDeviceInfo(p_d);
	if (info == nullptr) {
		return;
	}
	const int in = (int)info->maxInputChannels;
	const int out = (int)info->maxOutputChannels;
	if (p_want_input ? in > 0 : out > 0) {
		p_out.push_back({p_d, info->name, in, out});
	}
}

} // namespace

PortAudioPort::PortAudioPort() = default;

PortAudioPort::~PortAudioPort() {
	close();
}

int PortAudioPort::pa_cb(const void *p_in, void *p_out, unsigned long p_frames,
		const PaStreamCallbackTimeInfo *p_time_info, PaStreamCallbackFlags p_status_flags,
		void *p_user_data) {
	(void)p_time_info;
	(void)p_status_flags;
	PortAudioPort *self = static_cast<PortAudioPort *>(p_user_data);
	// render_ contract: dev_in may be nullptr (n_ins == 0); dev_out is
	// the device output buffer the callback must fill. No zero-init here
	// — the installed callback owns its output content (Task 3).
	self->render_(static_cast<float *>(const_cast<void *>(p_in)), static_cast<float *>(p_out),
			(int)p_frames);
	return paContinue;
}

int PortAudioPort::open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) {
	ensure_portaudio_initialized();
	if (stream_ != nullptr) {
		return -1; // already open
	}
	if (p_n_ins < 0 || p_n_out <= 0 || p_samplerate <= 0 || p_blocksize <= 0 ||
			p_blocksize % 64 != 0) {
		return -1;
	}

	// Output device: default, or the first output-capable device.
	PaDeviceIndex out_dev = Pa_GetDefaultOutputDevice();
	if (out_dev < 0) {
		const int count = Pa_GetDeviceCount();
		for (int d = 0; d < count; d++) {
			const PaDeviceInfo *info = Pa_GetDeviceInfo(d);
			if (info != nullptr && info->maxOutputChannels > 0) {
				out_dev = d;
				break;
			}
		}
	}
	if (out_dev < 0) {
		return -1;
	}

	// Input device: default when input is requested, none otherwise.
	PaDeviceIndex in_dev = paNoDevice;
	if (p_n_ins > 0) {
		in_dev = Pa_GetDefaultInputDevice();
		if (in_dev < 0) {
			return -1;
		}
	}

	PaStreamParameters in_params;
	in_params.device = in_dev;
	in_params.channelCount = p_n_ins;
	in_params.sampleFormat = paFloat32;
	in_params.suggestedLatency = 0.0; // host default
	in_params.hostApiSpecificStreamInfo = nullptr;

	PaStreamParameters out_params;
	out_params.device = out_dev;
	out_params.channelCount = p_n_out;
	out_params.sampleFormat = paFloat32;
	out_params.suggestedLatency = 0.0; // host default
	out_params.hostApiSpecificStreamInfo = nullptr;

	const PaError err = Pa_OpenStream(&stream_, in_dev == paNoDevice ? nullptr : &in_params,
			&out_params, (double)p_samplerate, p_blocksize, paNoFlag,
			&PortAudioPort::pa_cb, this);
	if (err != paNoError) {
		stream_ = nullptr;
		return -1;
	}
	// This vendored PortAudio leaves the stream stopped after Pa_OpenStream
	// (spike/native_audio/spike.c starts it explicitly too).
	if (Pa_StartStream(stream_) != paNoError) {
		Pa_CloseStream(stream_);
		stream_ = nullptr;
		return -1;
	}
	// This vendored PortAudio predates the Pa_OpenStream streamInfo out-param;
	// latencies come from Pa_GetStreamInfo.
	const PaStreamInfo *stream_info = Pa_GetStreamInfo(stream_);
	if (stream_info != nullptr) {
		output_latency_ms_ = stream_info->outputLatency * 1000.0;
		input_latency_ms_ = stream_info->inputLatency * 1000.0;
	}
	open_ = true;
	return 0;
}

void PortAudioPort::close() {
	if (stream_ != nullptr) {
		Pa_CloseStream(stream_);
		stream_ = nullptr;
	}
	open_ = false;
	output_latency_ms_ = 0.0;
	input_latency_ms_ = 0.0;
}

bool PortAudioPort::is_open() const {
	return open_;
}

std::vector<AudioDeviceInfo> PortAudioPort::list_inputs() const {
	std::vector<AudioDeviceInfo> out;
	ensure_portaudio_initialized();
	const int count = Pa_GetDeviceCount();
	for (int d = 0; d < count; d++) {
		add_device_if_capable(out, d, true);
	}
	return out;
}

std::vector<AudioDeviceInfo> PortAudioPort::list_outputs() const {
	std::vector<AudioDeviceInfo> out;
	ensure_portaudio_initialized();
	const int count = Pa_GetDeviceCount();
	for (int d = 0; d < count; d++) {
		add_device_if_capable(out, d, false);
	}
	return out;
}

double PortAudioPort::output_latency_ms() const {
	return open_ ? output_latency_ms_ : 0.0;
}

double PortAudioPort::input_latency_ms() const {
	return open_ ? input_latency_ms_ : 0.0;
}

bool PortAudioPort::supports_input() const {
	ensure_portaudio_initialized();
	return Pa_GetDefaultInputDevice() >= 0;
}

} // namespace godot_libpd

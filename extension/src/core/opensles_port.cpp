#include "opensles_port.h"

#include <algorithm>

namespace godot_libpd {

namespace {
constexpr int OUT_CHANNELS = 2;

SLuint32 samplingrate_constant(int p_samplerate) {
	if (p_samplerate == 44100) {
		return SL_SAMPLINGRATE_44_1;
	}
	if (p_samplerate == 48000) {
		return SL_SAMPLINGRATE_48;
	}
	return 0;
}
} // namespace

void OpenSLESPort::buffer_queue_trampoline(SLBufferQueueItf p_caller, void *p_context) {
	(void)p_caller;
	// Buffer-queue callback: single thread (the OpenSL playback thread), no
	// synchronization needed for the ring/index state.
	OpenSLESPort *self = static_cast<OpenSLESPort *>(p_context);
	self->on_buffer_consumed();
}

void OpenSLESPort::on_buffer_consumed() {
	const int frames = frames_per_buffer();
	// 1) Render: the installed RenderFn fills the caller-allocated scratch
	// (the same contract PortAudioPort follows — NativeAudio renders the
	// mix-down here, on this thread).
	if (render_) {
		render_(nullptr, scratch_.data(), frames);
	}
	// 2) Convert float32 → int16 into the recycled buffer slot.
	short *dst = pcm_.data() + (size_t)ring_ * frames * OUT_CHANNELS;
	const float *src = scratch_.data();
	const size_t n = (size_t)frames * OUT_CHANNELS;
	for (size_t i = 0; i < n; i++) {
		float v = src[i];
		if (v > 1.0f) {
			v = 1.0f;
		} else if (v < -1.0f) {
			v = -1.0f;
		}
		dst[i] = (short)(v * 32767.0f);
	}
	// 3) Re-enqueue the recycled buffer (raw data pointer per the NDK API).
	SLresult r = (*queue_itf_)->Enqueue(queue_itf_, pcm_.data() + (size_t)ring_ * n, n * sizeof(short));
	if (r != SL_RESULT_SUCCESS) {
		enqueue_fails_++;
	}
	ring_ = (ring_ + 1) % buffer_count();
}

int OpenSLESPort::open(int p_n_ins, int p_n_out, int p_samplerate, int p_blocksize) {
	if (open_) {
		return 0;
	}
	if (p_n_ins != 0) {
		return 1; // output-only port
	}
	if (p_n_out != OUT_CHANNELS) {
		return 2; // stereo-only
	}
	if (p_blocksize != frames_per_buffer()) {
		return 3; // fixed 1024-frame buffers (device requirement)
	}
	SLuint32 rate_const = samplingrate_constant(p_samplerate);
	if (rate_const == 0) {
		return 4; // only 44100/48000 are expressible
	}
	const int frames = frames_per_buffer();
	const int nbuf = buffer_count();

	// Engine (threadsafe, like Godot's production driver).
	SLEngineOption opts[] = { { SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE } };
	SLresult r = slCreateEngine(&engine_, 1, opts, 0, 0, 0);
	if (r != SL_RESULT_SUCCESS) {
		return 5;
	}
	r = (*engine_)->Realize(engine_, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		destroy_engine_only();
		return 6;
	}
	r = (*engine_)->GetInterface(engine_, SL_IID_ENGINE, (void *)&engine_itf_);
	if (r != SL_RESULT_SUCCESS) {
		destroy_engine_only();
		return 7;
	}

	// Output mix.
	r = (*engine_itf_)->CreateOutputMix(engine_itf_, &outmix_, 0, 0, 0);
	if (r != SL_RESULT_SUCCESS) {
		destroy_engine_only();
		return 8;
	}
	r = (*outmix_)->Realize(outmix_, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		destroy_engine_only();
		return 9;
	}

	// Player: AndroidSimpleBufferQueue source (2 buffers) -> output mix.
	SLDataLocator_AndroidSimpleBufferQueue loc_src = {
			SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, (SLuint32)nbuf };
	SLDataLocator_OutputMix loc_out = { SL_DATALOCATOR_OUTPUTMIX, outmix_ };
	SLDataFormat_PCM pcm = {
			SL_DATAFORMAT_PCM,
			OUT_CHANNELS,
			rate_const,
			SL_PCMSAMPLEFORMAT_FIXED_16,
			SL_PCMSAMPLEFORMAT_FIXED_16,
			SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
			SL_BYTEORDER_LITTLEENDIAN,
	};
	SLDataSource src = { &loc_src, &pcm };
	SLDataSink snk = { &loc_out, 0 };
	// Plain BUFFERQUEUE only (the shape of Godot's production driver).
	const SLInterfaceID ids[1] = { SL_IID_BUFFERQUEUE };
	const SLboolean req[1] = { SL_BOOLEAN_TRUE };
	r = (*engine_itf_)->CreateAudioPlayer(engine_itf_, &player_, &src, &snk, 1, ids, req);
	if (r != SL_RESULT_SUCCESS) {
		destroy_engine_only();
		return 10;
	}
	r = (*player_)->Realize(player_, SL_BOOLEAN_FALSE);
	if (r != SL_RESULT_SUCCESS) {
		destroy_player_and_engine();
		return 11;
	}
	r = (*player_)->GetInterface(player_, SL_IID_PLAY, (void *)&play_itf_);
	if (r != SL_RESULT_SUCCESS) {
		destroy_player_and_engine();
		return 12;
	}
	r = (*player_)->GetInterface(player_, SL_IID_BUFFERQUEUE, (void *)&queue_itf_);
	if (r != SL_RESULT_SUCCESS) {
		destroy_player_and_engine();
		return 13;
	}
	r = (*queue_itf_)->RegisterCallback(queue_itf_, buffer_queue_trampoline, this);
	if (r != SL_RESULT_SUCCESS) {
		destroy_player_and_engine();
		return 14;
	}

	// Persistent buffers + render scratch.
	pcm_.assign((size_t)nbuf * frames * OUT_CHANNELS, 0);
	scratch_.assign((size_t)frames * OUT_CHANNELS, 0.0f);
	samplerate_ = p_samplerate;
	ring_ = 0;
	enqueue_fails_ = 0;
	// Prime both buffers (render_ may still be unbound → zeros), then start.
	for (int i = 0; i < nbuf; i++) {
		ring_ = i;
		on_buffer_consumed(); // render into slot i + enqueue
	}
	r = (*play_itf_)->SetPlayState(play_itf_, SL_PLAYSTATE_PLAYING);
	if (r != SL_RESULT_SUCCESS) {
		destroy_player_and_engine();
		return 15;
	}
	open_ = true;
	return 0;
}

void OpenSLESPort::close() {
	if (!open_) {
		return;
	}
	open_ = false;
	if (play_itf_ != nullptr) {
		(*play_itf_)->SetPlayState(play_itf_, SL_PLAYSTATE_STOPPED);
	}
	destroy_player_and_engine();
}

bool OpenSLESPort::is_open() const {
	return open_;
}

std::vector<AudioDeviceInfo> OpenSLESPort::list_inputs() const {
	return {};
}

std::vector<AudioDeviceInfo> OpenSLESPort::list_outputs() const {
	AudioDeviceInfo dev;
	dev.index = 0;
	dev.name = "OpenSL ES output";
	dev.max_out = OUT_CHANNELS;
	return { dev };
}

double OpenSLESPort::output_latency_ms() const {
	if (!open_ || samplerate_ <= 0) {
		return 0.0;
	}
	// Best-effort: the two-buffer pipeline depth.
	return (double)buffer_count() * frames_per_buffer() / (double)samplerate_ * 1000.0;
}

OpenSLESPort::~OpenSLESPort() {
	close();
}

} // namespace godot_libpd

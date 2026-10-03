// native_audio_spike: de-risking spike for the v2 M5/M6 native-audio design
// (docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md).
//
// Validates the "Approach A" model on macOS with the vendored PortAudio +
// vendored libpd:
//   - the PortAudio real-time callback drives libpd_process_float directly
//     (ticks = frames / libpd_blocksize());
//   - a chosen block size is honored by the device;
//   - audio input reaches the callback (mic capture);
//   - measured latency floor (device latency + one buffer).
//
// No Godot, no worker thread, no ring buffer — the callback IS the clock.
//
// usage:
//   native_audio_spike [blocksize] [samplerate] <patch.pd> [noteon]
//   native_audio_spike 256 44100 sine.pd
//   native_audio_spike 128 44100 loopback.pd
// Ctrl-C to stop. Prints a status line every second.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <portaudio.h>
#include <z_libpd.h>

static volatile sig_atomic_t running = 1;
static void on_sig(int sig) { (void)sig; running = 0; }

// state shared with the callback (written on the RT thread, read by main).
struct Spike {
	int n_in;
	int n_out;
	t_pdinstance *inst;
	volatile unsigned long last_frames; // actual callback block size (device)
};
static struct Spike g;
static volatile float out_peak = 0.0f;
static volatile float in_peak = 0.0f;
static volatile unsigned long long blocks_rendered = 0;

static void print_hook(const char *s) {
	if (s != NULL) {
		printf("%s", s);
		fflush(stdout);
	}
}

static int pa_callback(const void *in, void *out, unsigned long frames,
		const PaStreamCallbackTimeInfo *timeInfo,
		PaStreamCallbackFlags statusFlags, void *userData) {
	(void)timeInfo;
	(void)statusFlags;
	(void)userData;

	// pd_this is THREAD-LOCAL (m_class.c: PERTHREAD). Every thread that
	// touches libpd must set the current instance first. The v1 worker
	// got this for free (set_instance + process_float on one thread);
	// Approach A must do it here on the audio thread.
	libpd_set_instance(g.inst);
	g.last_frames = frames;

	const int bs = libpd_blocksize();
	const int ticks = (int)(frames / bs);
	if (ticks > 0) {
		// The core of Approach A: libpd renders directly into the
		// PortAudio output buffer from the PortAudio input buffer.
		libpd_process_float(ticks, (const float *)in, (float *)out);
		blocks_rendered += (unsigned long long)ticks;
	}

	// Output peak (interleaved, n_out channels).
	if (out != NULL && g.n_out > 0) {
		const float *o = (const float *)out;
		float peak = 0.0f;
		const long n = (long)frames * g.n_out;
		for (long i = 0; i < n; ++i) {
			const float a = o[i] < 0.0f ? -o[i] : o[i];
			if (a > peak) peak = a;
		}
		if (peak > out_peak) out_peak = peak;
	}
	// Input peak (device capture reaching the callback).
	if (in != NULL && g.n_in > 0) {
		const float *i = (const float *)in;
		float peak = 0.0f;
		const long n = (long)frames * g.n_in;
		for (long j = 0; j < n; ++j) {
			const float a = i[j] < 0.0f ? -i[j] : i[j];
			if (a > peak) peak = a;
		}
		if (peak > in_peak) in_peak = peak;
	}
	return paContinue;
}

static void list_devices(void) {
	const int count = Pa_GetDeviceCount();
	printf("devices:\n");
	for (int d = 0; d < count; ++d) {
		const PaDeviceInfo *info = Pa_GetDeviceInfo(d);
		if (info == NULL) continue;
		printf("  [%2d] in=%d out=%d  %s\n", d, info->maxInputChannels,
				info->maxOutputChannels, info->name);
	}
}

int main(int argc, char **argv) {
	int blocksize = 256;
	int samplerate = 44100;
	const char *patch = NULL;
	int send_note = 0;

	if (argc >= 2) blocksize = atoi(argv[1]);
	if (argc >= 3) samplerate = atoi(argv[2]);
	if (argc >= 4) patch = argv[3];
	if (argc >= 5) send_note = 1;
	if (patch == NULL) {
		fprintf(stderr, "usage: %s [blocksize] [samplerate] <patch.pd> [noteon]\n",
				argv[0]);
		return 2;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (Pa_Initialize() != paNoError) {
		fprintf(stderr, "Pa_Initialize failed\n");
		return 1;
	}
	printf("portaudio: %s\n", Pa_GetVersionText());
	list_devices();

	const int outdev = Pa_GetDefaultOutputDevice();
	const int indev = Pa_GetDefaultInputDevice();
	int n_out = 2;
	int n_in = 0;
	if (outdev >= 0) {
		const PaDeviceInfo *o = Pa_GetDeviceInfo(outdev);
		if (o != NULL && o->maxOutputChannels >= 2) n_out = 2;
	}
	if (indev >= 0) {
		const PaDeviceInfo *i = Pa_GetDeviceInfo(indev);
		if (i != NULL && i->maxInputChannels >= 2) n_in = 2; else if (i != NULL) n_in = 1;
	}
	g.n_in = n_in;
	g.n_out = n_out;
	printf("using: out dev=%d (%d ch) in dev=%d (%d ch) rate=%d block=%d\n",
			outdev, n_out, indev, n_in, samplerate, blocksize);

	PaStreamParameters ip = { indev, n_in, paFloat32, 0, NULL };
	PaStreamParameters op = { outdev, n_out, paFloat32, 0, NULL };
	PaStream *stream = NULL;
	PaError err = Pa_OpenStream(&stream,
			(n_in > 0) ? &ip : NULL, &op,
			samplerate, blocksize, 0, pa_callback, &g);
	if (err != paNoError) {
		fprintf(stderr, "Pa_OpenStream: %s\n", Pa_GetErrorText(err));
		Pa_Terminate();
		return 1;
	}
	const PaStreamInfo *si = Pa_GetStreamInfo(stream);
	const double buf_ms = 1000.0 * ((double)blocksize / (double)samplerate);
	printf("stream: block=%d rate=%.0f device-out-latency=%.3f ms  (floor ~= device + one buffer = %.3f ms)\n",
			(int)blocksize, si->sampleRate, 1000.0 * si->outputLatency,
			1000.0 * si->outputLatency + buf_ms);

	// libpd: init, audio, dsp on, load patch — all on the main thread
	// before the stream starts (mirrors pdtest_portaudio.c).
	libpd_set_printhook(print_hook);
	libpd_init();
	t_pdinstance *inst = libpd_new_instance();
	libpd_set_instance(inst);
	g.inst = inst;
	libpd_init_audio(n_in, n_out, samplerate);
	printf("libpd_blocksize()=%d\n", libpd_blocksize());
	libpd_start_message(1);
	libpd_add_float(1.0f);
	libpd_finish_message("pd", "dsp");

	const size_t slash = strrchr(patch, '/') ? (size_t)(strrchr(patch, '/') - patch) : (size_t)-1;
	const char *file = (slash == (size_t)-1) ? patch : patch + slash + 1;
	const char *dir = (slash == (size_t)-1) ? "." : patch;
	if (!libpd_openfile(file, dir)) {
		fprintf(stderr, "libpd_openfile failed for %s\n", patch);
		Pa_StopStream(stream);
		Pa_Terminate();
		return 1;
	}
	if (send_note) {
		libpd_noteon(0, 60, 100);
		printf("sent noteon ch0 pitch60 vel100\n");
	}

	err = Pa_StartStream(stream);
	if (err != paNoError) {
		fprintf(stderr, "Pa_StartStream: %s\n", Pa_GetErrorText(err));
		return 1;
	}
	printf("stream started; rendering in real time (Ctrl-C to stop)\n");

	// Status loop: one line per second.
	while (running) {
		for (int i = 0; i < 10 && running; ++i) {
			struct timespec ts = { 0, 100 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		if (!running) break;
		printf("  out_peak=%.4f in_peak=%.4f actual_block=%lu blocks=%llu (frames=%llu)\n",
				out_peak, in_peak, g.last_frames, blocks_rendered,
				blocks_rendered * (unsigned long long)libpd_blocksize());
	}

	Pa_StopStream(stream);
	Pa_CloseStream(stream);
	printf("stopped. total blocks=%llu (added latency ~= one buffer = %.3f ms at block=%d/rate=%d)\n",
			blocks_rendered,
			1000.0 * ((double)blocksize / (double)samplerate),
			blocksize, samplerate);
	Pa_Terminate();
	libpd_free_instance(inst);
	return 0;
}

// rt_repro.cpp — isolates the M5 "mix rendered on the PortAudio callback"
// design under AddressSanitizer. 8 paced synth workers + 1 mix-down instance
// rendered in a REAL CoreAudio PortAudio callback (the current app design).
// If the RT callback's libpd_process_float (high overlap with the 8 synth
// workers) is the heap-corruption culprit, ASan reports the OOB write here
// and does NOT report it in the "mix on a paced thread" variant.
//
// Build: see the comment block; -fsanitize=address, real PortAudio CoreAudio.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "portaudio.h"

extern "C" {
#include "z_libpd.h"
}

static const char *kSine =
		"#N canvas 0 0 200 200 12;\n"
		"#X obj 20 20 osc~ 220;\n"
		"#X obj 20 90 *~ 0.3;\n"
		"#X obj 20 150 dac~;\n"
		"#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n#X connect 1 0 2 1;\n";
static const char *kAdc1 =
		"#N canvas 0 0 400 300 12;\n"
		"#X obj 20 20 adc~ 1;\n#X obj 20 60 *~ 1.0;\n#X obj 20 120 dac~;\n"
		"#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n#X connect 1 0 2 1;\n";

// Minimal ring (mirrors MixInputRing Option B).
struct Ring {
	int ch = 2, bs = 64, nb = 8;
	std::vector<float> st;
	mutable std::mutex mu;
	unsigned long long count = 0;
	Ring(int c, int b, int n) : ch(c), bs(b), nb(n), st((size_t)n * b * c, 0.0f) {}
	void push(const float *p, int f) {
		std::lock_guard<std::mutex> l(mu);
		std::copy_n(p, (size_t)f * ch, st.data() + (count % nb) * (size_t)bs * ch);
		count++;
	}
	void gather_n(float *dst, int nblocks) const {
		int n = nblocks < nb ? nblocks : nb;
		std::fill_n(dst, (size_t)n * bs * ch, 0.0f);
		std::lock_guard<std::mutex> l(mu);
		unsigned long long avail = count >= (unsigned long long)nb ? (unsigned long long)nb : count;
		int copy = (int)(avail < (unsigned long long)n ? avail : (unsigned long long)n);
		if (copy <= 0)
			return;
		int oldest = (int)(((count - copy) % nb));
		int base = n - copy;
		for (int i = 0; i < copy; i++) {
			int slot = (oldest + i) % nb;
			std::copy_n(st.data() + (size_t)slot * bs * ch, (size_t)bs * ch, dst + (size_t)(base + i) * bs * ch);
		}
	}
};

static void write_patch(const char *n, const char *t) {
	std::ofstream f(std::string("/tmp/") + n);
	f << t;
}

// 8 synth worker threads (paced, a1 model).
struct Synth {
	std::thread th;
	t_pdinstance *pd = nullptr;
	Ring *ring = nullptr;
	std::atomic<bool> stop{false};
};
static void synth_main(Synth &s, const std::string &patch) {
	s.pd = libpd_new_instance();
	libpd_set_instance(s.pd);
	libpd_init_audio(0, 2, 44100);
	libpd_start_message(1);
	libpd_add_float(1.0f);
	libpd_finish_message("pd", "dsp");
	libpd_openfile(patch.c_str(), "/tmp");
	std::vector<float> out(64 * 2, 0.0f);
	auto next = std::chrono::steady_clock::now();
	while (!s.stop.load()) {
		libpd_process_float(1, nullptr, out.data());
		s.ring->push(out.data(), 64);
		std::this_thread::sleep_for(std::chrono::duration<double>(64.0 / 44100.0));
	}
}

// Globals for the callback.
static std::vector<Ring *> g_rings;
static std::atomic<t_pdinstance *> g_mix{nullptr};
static std::atomic<bool> g_mix_set{false};
static std::mutex g_render_lock;
static int g_blocksize = 256;
static std::vector<float> g_mix_in, g_mix_out, g_scratch;

static int pa_callback(const void *in, void *out, unsigned long frames, const PaStreamCallbackTimeInfo *t, PaStreamCallbackFlags f, void *ud) {
	(void)in; (void)t; (void)f; (void)ud;
	int n = 256;
	if (n > g_blocksize) n = g_blocksize;
	int nin = 16;
	std::fill_n(g_mix_in.data(), (size_t)n * nin, 0.0f);
	int k = n / 64;
	int i = 0;
	for (Ring *r : g_rings) {
		if (2 * i + 1 >= nin) break;
		r->gather_n(g_scratch.data(), k);
		const float *sc = g_scratch.data();
		for (int ff = 0; ff < n; ff++) {
			g_mix_in[(size_t)ff * nin + 2 * i] = sc[2 * ff];
			g_mix_in[(size_t)ff * nin + 2 * i + 1] = sc[2 * ff + 1];
		}
		i++;
	}
	std::lock_guard<std::mutex> lk(g_render_lock);
	t_pdinstance *mix = g_mix.load();
	float *dev_out = (float *)out;
	if (mix == nullptr) {
		std::fill_n(dev_out, (size_t)n * 2, 0.0f);
		return paContinue;
	}
	if (!g_mix_set.exchange(true))
		libpd_set_instance(mix);
	int ticks = n / 64;
	libpd_process_float(ticks, g_mix_in.data(), g_mix_out.data());
	std::copy_n(g_mix_out.data(), (size_t)n * 2, dev_out);
	return paContinue;
}

int main() {
	if (libpd_init() != 0) {
		printf("libpd_init failed\n");
		return 1;
	}
	write_patch("rt_synth.pd", kSine);
	write_patch("rt_mix.pd", kAdc1);

	g_blocksize = 256;
	g_mix_in.assign((size_t)256 * 16, 0.0f);
	g_mix_out.assign((size_t)256 * 2, 0.0f);
	g_scratch.assign(2 * 256, 0.0f);

	// 8 synth rings + workers.
	std::vector<std::unique_ptr<Ring>> rg(8);
	std::vector<Synth> synths(8);
	for (int i = 0; i < 8; i++) {
		rg[i] = std::make_unique<Ring>(2, 64, 8);
		synths[i].ring = rg[i].get();
		g_rings.push_back(rg[i].get());
		synths[i].th = std::thread(synth_main, std::ref(synths[i]), std::string("rt_synth.pd"));
	}

	// Mix instance: init on the main thread (rendered on the callback thread).
	t_pdinstance *mix = libpd_new_instance();
	libpd_set_instance(mix);
	libpd_init_audio(16, 2, 44100);
	libpd_start_message(1);
	libpd_add_float(1.0f);
	libpd_finish_message("pd", "dsp");
	libpd_openfile("rt_mix.pd", "/tmp");
	g_mix.store(mix);

	// Real PortAudio CoreAudio stream (0 in, 2 out).
	if (Pa_Initialize() != paNoError) {
		printf("Pa_Initialize failed\n");
		return 1;
	}
	PaStream *stream = nullptr;
	PaStreamParameters out;
	out.device = Pa_GetDefaultOutputDevice();
	out.channelCount = 2;
	out.sampleFormat = paFloat32;
	out.suggestedLatency = 0.0;
	out.hostApiSpecificStreamInfo = nullptr;
	PaError err = Pa_OpenStream(&stream, nullptr, &out, 44100, 256, paNoFlag, pa_callback, nullptr);
	if (err != paNoError) {
		printf("Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
		return 1;
	}
	Pa_StartStream(stream);

	printf("rt_repro: running (8 synth + mix on real CoreAudio callback); ~6 s\n");
	std::this_thread::sleep_for(std::chrono::seconds(6));

	Pa_StopStream(stream);
	Pa_CloseStream(stream);
	for (auto &s : synths) {
		s.stop = true;
		s.th.join();
	}
	libpd_set_instance(mix);
	libpd_closefile(nullptr);
	libpd_free_instance(mix);
	Pa_Terminate();
	printf("rt_repro: clean exit\n");
	return 0;
}

// Unit tests for PortAudioPort (M5 Task 2) — device enumeration,
// stream open/close through the static C-callback trampoline, and
// latency reporting from PaStreamInfo.
// Plain assert-style harness: any failure prints and exits non-zero.
// Hosts with no audio device print "SKIP (no audio)" and exit 0 so
// headless CI stays green.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "portaudio.h"

#include "core/portaudio_port.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	// Graceful skip on device-less hosts (headless CI).
	if (Pa_Initialize() != paNoError || Pa_GetDeviceCount() == 0 ||
			Pa_GetDefaultOutputDevice() < 0) {
		std::printf("SKIP (no audio)\n");
		return 0;
	}

	PortAudioPort port;

	// 1. Init + enum: at least one output-capable device on this host.
	const std::vector<AudioDeviceInfo> outputs = port.list_outputs();
	CHECK(outputs.size() >= 1);
	for (const AudioDeviceInfo &d : outputs) {
		CHECK(d.index >= 0);
		CHECK(d.max_out > 0);
		CHECK(!d.name.empty());
	}
	std::printf("list_outputs: %zu device(s)\n", outputs.size());

	// 2. Latency sane: non-negative (0.0 while closed).
	CHECK(port.output_latency_ms() >= 0.0);
	CHECK(port.input_latency_ms() >= 0.0);

	// 3. Open + close: a render callback writing 0.5 proves the
	//    trampoline invoked render_ on the audio thread.
	std::atomic<float> peak{0.0f};
	port.set_render_callback([&peak](float *p_dev_in, float *p_dev_out, int p_frames) {
		(void)p_dev_in; // nullptr: n_ins == 0
		for (int i = 0; i < p_frames * 2; i++) {
			p_dev_out[i] = 0.5f;
		}
		peak = 0.5f;
	});
	CHECK(port.open(0 /* n_ins */, 2 /* n_out */, 44100, 256 /* blocksize */) == 0);
	CHECK(port.is_open());
	const double out_latency_ms = port.output_latency_ms();
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	CHECK(peak > 0.4f);
	port.close();
	CHECK(!port.is_open());
	std::printf("open_close done (peak=%.3f out_latency_ms=%.1f)\n",
			(double)peak.load(), out_latency_ms);

	// 4. Bad blocksize rejected (not a multiple of 64).
	CHECK(port.open(0, 2, 44100, 100 /* blocksize */) == -1);
	CHECK(!port.is_open());

	if (failures == 0) {
		std::printf("ALL PORTAUDIO PORT TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

// Unit tests for AudioPort / NullPort / mix_block (Task 1).
// Plain assert-style harness: any failure prints and exits non-zero.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/audio_port.h"
#include "core/null_port.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

static void test_mix_single_input() {
	std::vector<float> in(4, 0.0f);
	in[0] = 0.25f;
	in[1] = -0.25f;
	in[2] = 0.5f;
	in[3] = 0.0f;
	const float *inputs[1] = {in.data()};
	std::vector<float> out(4, 123.0f);
	mix_block(out.data(), 2 /* out_ch */, inputs, 1 /* n_inputs */, 2 /* frames */);
	CHECK(out[0] == 0.25f);
	CHECK(out[1] == -0.25f);
	CHECK(out[2] == 0.5f);
	CHECK(out[3] == 0.0f);
	std::printf("mix_single_input done\n");
}

static void test_mix_two_inputs_sums() {
	const float a[2] = {0.5f, 0.0f};
	const float b[2] = {0.25f, -0.5f};
	const float *inputs[2] = {a, b};
	std::vector<float> out(2, 0.0f);
	mix_block(out.data(), 2 /* out_ch */, inputs, 2 /* n_inputs */, 1 /* frames */);
	CHECK(out[0] == 0.75f);
	CHECK(out[1] == -0.5f);
	std::printf("mix_two_inputs_sums done\n");
}

static void test_mix_clamps_to_unity() {
	const float a[1] = {1.0f};
	const float b[1] = {0.5f};
	const float *inputs[2] = {a, b};
	std::vector<float> out(1, 0.0f);
	mix_block(out.data(), 1 /* out_ch */, inputs, 2 /* n_inputs */, 1 /* frames */);
	CHECK(out[0] == 1.0f); // clamped, not 1.5, not wrapped

	const float a_neg[1] = {-1.0f};
	const float b_neg[1] = {-0.5f};
	const float *inputs_neg[2] = {a_neg, b_neg};
	mix_block(out.data(), 1 /* out_ch */, inputs_neg, 2 /* n_inputs */, 1 /* frames */);
	CHECK(out[0] == -1.0f); // negative clamp
	std::printf("mix_clamps_to_unity done\n");
}

static void test_mix_zero_inputs() {
	// n_inputs == 0: mix_block zeroes every sample (impl decision, pinned here).
	std::vector<float> out(2, 9.0f);
	const float *inputs[1] = {nullptr};
	mix_block(out.data(), 2 /* out_ch */, inputs, 0 /* n_inputs */, 1 /* frames */);
	CHECK(out[0] == 0.0f);
	CHECK(out[1] == 0.0f);
	std::printf("mix_zero_inputs done\n");
}

static void test_null_port_open_and_render() {
	NullPort port;
	port.set_render_callback([](float *p_dev_in, float *p_dev_out, int p_frames) {
		(void)p_dev_in;
		const int samples = p_frames * 2; // n_out == 2
		for (int i = 0; i < samples; i++) {
			p_dev_out[i] = 0.5f;
		}
	});
	CHECK(port.open(1 /* n_ins */, 2 /* n_out */, 44100, 256 /* blocksize */) == 0);
	CHECK(port.is_open());
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	CHECK(port.frames_rendered() > 0);
	CHECK(port.last_out_peak() > 0.4f);
	port.close();
	CHECK(!port.is_open());
	std::printf("null_port_open_and_render done (frames=%llu peak=%.3f)\n",
			(unsigned long long)port.frames_rendered(), (double)port.last_out_peak());
}

static void test_null_port_blocksize_honored() {
	NullPort port;
	std::atomic<int> recorded_frames{-1};
	port.set_render_callback([&recorded_frames](float *p_dev_in, float *p_dev_out, int p_frames) {
		(void)p_dev_in; // nullptr: n_ins == 0
		(void)p_dev_out;
		recorded_frames = p_frames;
	});
	CHECK(port.open(0 /* n_ins */, 2 /* n_out */, 44100, 128 /* blocksize */) == 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	CHECK(recorded_frames == 128);
	port.close();
	std::printf("null_port_blocksize_honored done (recorded_frames=%d)\n",
			recorded_frames.load());
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_mix_single_input();
	test_mix_two_inputs_sums();
	test_mix_clamps_to_unity();
	test_mix_zero_inputs();
	test_null_port_open_and_render();
	test_null_port_blocksize_honored();

	if (failures == 0) {
		std::printf("ALL AUDIO PORT TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

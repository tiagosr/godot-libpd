// Unit tests for M5 Task 6 + Task 8 rework — server audio API +
// per-instance ring wiring + dedicated mix render thread.
//
// Option B (supervisor-approved): the per-instance MixInputRing blocksize is
// libpd_blocksize() (64 in this vendored build) — what the worker pushes per
// tick — and the PortAudio stream blocksize (e.g. 256) is a multiple of it.
// The DEDICATED mix render thread gathers the latest K = stream_blocksize /
// ring_blocksize blocks per ring (MixInputRing::gather_latest_n,
// oldest-first) each stream block; the real-time callback only DRAINS the
// stereo mix-output ring (latest block, silence when unbound — no libpd,
// no lock). This file pins that contract:
//
//   1. gather_latest_n — deterministic ordering / zero-fill / return value
//      (pure ring logic, no threads).
//   2. NativeAudio::open blocksize validation (multiple of libpd_blocksize).
//   3. NativeAudio::set_mixer channel-count validation + one-binding
//      contract + clear_mixer rebind (invariants #1 and #3 from the T4/T5
//      reviews) + has_mixer() state.
//   4. register_worker_ring divisibility contract (invariant #4): a 64-frame
//      ring registers into a 256-frame stream; non-divisors and a second
//      ring blocksize are rejected.
//   5. K-gather end-to-end: 64-frame pushes render full-rate 256-frame
//      blocks (no stretching, no starvation).
//   6. Full production SYNTH path: real LibpdWorker -> MixInputRing(64)
//      -> NativeAudio(256) -> mix instance on the dedicated mix thread.
//   7. Invariant #1: clear_mixer() stops + joins the mix render thread,
//      so the mixer instance can be freed while the audio thread is still
//      live — the drain renders silence afterwards instead of a dangling
//      pointer.
//
// The LibpdServer / LibpdInstance Godot nodes cannot be constructed in a
// host test (GDExtension interface pointers are null outside the engine),
// so the server-level glue (port ownership, role validation, teardown
// ordering) is compile-checked by the extension build and exercised on
// device (T7/T8). Same harness as the T4/T5 tests: standalone main +
// CHECK, real libpd instances, NullPort as the device-free audio thread.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "core/mix_input_ring.h"
#include "core/native_audio.h"
#include "core/null_port.h"
#include "libpd_worker.h"

// libpd multi-instance API.
extern "C" {
#include "z_libpd.h"
}

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

static void sleep_ms(int p_ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(p_ms));
}

/** Writes p_text to /tmp/p_name (truncating any pre-existing file). */
static void write_patch(const char *p_name, const char *p_text) {
	std::ofstream f(std::string("/tmp/") + p_name);
	f << p_text;
}

static void turn_dsp_on() {
	libpd_start_message(1);
	libpd_add_float(1.0f);
	libpd_finish_message("pd", "dsp");
}

// [adc~ 1] *~ 1.0 [dac~] — passes mix input channel 1 to both outputs.
static const char *const kAdc1Patch =
		"#N canvas 0 0 400 300 12;\n"
		"#X obj 20 20 adc~ 1;\n"
		"#X obj 20 60 *~ 1.0;\n"
		"#X obj 20 120 dac~;\n"
		"#X connect 0 0 1 0;\n"
		"#X connect 1 0 2 0;\n"
		"#X connect 1 0 2 1;\n";
// [osc~ 200] *~ 0.3 [dac~] — self-contained 2ch sine (mix or synth patch).
static const char *const kSinePatch =
		"#N canvas 0 0 200 200 12;\n"
		"#X obj 20 20 osc~ 200;\n"
		"#X obj 20 90 *~ 0.3;\n"
		"#X obj 20 150 dac~;\n"
		"#X connect 0 0 1 0;\n"
		"#X connect 1 0 2 0;\n"
		"#X connect 1 0 2 1;\n";

/** A real libpd instance created + init'd on the calling (test) thread. */
struct PdInstance {
	t_pdinstance *pd = nullptr;
	void *file_handle = nullptr;
};

static PdInstance init_pd_instance(int p_n_in, int p_n_out, const char *p_patch) {
	PdInstance inst;
	inst.pd = libpd_new_instance();
	libpd_set_instance(inst.pd);
	libpd_init_audio(p_n_in, p_n_out, 44100);
	turn_dsp_on();
	inst.file_handle = libpd_openfile(p_patch, "/tmp");
	return inst;
}

/**
 * Tear down a test-thread instance. In test 7 this runs WHILE the audio
 * (drain) thread is live — legal only because clear_mixer() has stopped
 * and joined the mix render thread, so no thread references the instance
 * anymore (invariant #1, Task 8 rework).
 */
static void pd_instance_teardown(PdInstance &p_inst) {
	if (p_inst.pd == nullptr) {
		return;
	}
	libpd_set_instance(p_inst.pd);
	if (p_inst.file_handle != nullptr) {
		libpd_closefile(p_inst.file_handle);
		p_inst.file_handle = nullptr;
	}
	libpd_free_instance(p_inst.pd);
	p_inst.pd = nullptr;
}

/**
 * Producer thread pushing 64-frame blocks (the worker's tick size) into
 * p_ring at worker pacing (~1.45 ms per block at 44100 Hz).
 */
static std::thread start_ring_feeder(MixInputRing &p_ring, std::atomic<bool> &p_stop,
		float p_value = 0.5f) {
	return std::thread([&p_ring, &p_stop, p_value]() {
		std::vector<float> block(64 * 2, p_value);
		while (!p_stop.load(std::memory_order_relaxed)) {
			p_ring.push(block.data(), 64);
			std::this_thread::sleep_for(std::chrono::microseconds(1450));
		}
	});
}

// Worker command helpers (same pattern as native_worker_tests.cpp).
static const int k_command_timeout = std::numeric_limits<int>::min();

static int push_and_wait(LibpdWorker &p_worker, const PdCommand &p_command, int p_timeout_ms = 5000) {
	auto promise = std::make_shared<std::promise<int>>();
	auto future = promise->get_future();
	PdCommand command = p_command;
	command.result = &promise; // v1 convention: shared_ptr* reinterpreted
	p_worker.push_command(command);
	if (future.wait_for(std::chrono::milliseconds(p_timeout_ms)) != std::future_status::ready) {
		CHECK(false && "command never fulfilled");
		return k_command_timeout;
	}
	return future.get();
}

static int init_worker(LibpdWorker &p_worker, int p_n_ins, int p_n_out) {
	PdCommand init;
	init.opcode = PdCommand::INIT;
	init.i32 = 44100;
	init.i64 = (int64_t)p_n_ins * 1000 + p_n_out;
	return push_and_wait(p_worker, init);
}

static int load_patch(LibpdWorker &p_worker, const std::string &p_patch) {
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = p_patch.c_str();
	load.search = "/tmp";
	return push_and_wait(p_worker, load);
}

// 1. gather_latest_n — deterministic ring contract (Option B).
static void test_gather_latest_n() {
	MixInputRing ring(2, 64, 8);
	std::vector<float> dst(4 * 64 * 2, -1.0f);

	// Four pushes -> the four blocks, oldest-first.
	for (int k = 1; k <= 4; ++k) {
		std::vector<float> block(64 * 2, 0.1f * k);
		ring.push(block.data(), 64);
	}
	CHECK(ring.gather_latest_n(dst.data(), 4) == 4);
	for (int b = 0; b < 4; ++b) {
		const float want = 0.1f * (b + 1);
		bool ok = true;
		for (int s = 0; s < 64 * 2; ++s) {
			if (dst[b * 64 * 2 + s] != want) {
				ok = false;
				break;
			}
		}
		CHECK(ok && "oldest-first order, block values intact");
	}

	// Two more pushes -> the window slides (blocks 3..6).
	for (int k = 5; k <= 6; ++k) {
		std::vector<float> block(64 * 2, 0.1f * k);
		ring.push(block.data(), 64);
	}
	CHECK(ring.gather_latest_n(dst.data(), 4) == 4);
	for (int b = 0; b < 4; ++b) {
		CHECK(dst[b * 64 * 2] == 0.1f * (b + 3));
	}

	// Four more pushes (10 total) -> the ring is older than its depth:
	// the window caps at num_blocks() (8) and holds blocks 3..10. The dst
	// must hold the REQUESTED 12 blocks (p_n_blocks * blocksize * channels)
	// even though only 8 are written.
	std::vector<float> big(12 * 64 * 2, -1.0f);
	for (int k = 7; k <= 10; ++k) {
		std::vector<float> block(64 * 2, 0.1f * k);
		ring.push(block.data(), 64);
	}
	CHECK(ring.gather_latest_n(big.data(), 12) == 8);
	for (int b = 0; b < 8; ++b) {
		CHECK(big[b * 64 * 2] == 0.1f * (b + 3));
	}

	// Fresh ring: nothing available -> returns 0, dst fully zeroed.
	MixInputRing fresh(2, 64, 8);
	std::fill(dst.begin(), dst.end(), -1.0f);
	CHECK(fresh.gather_latest_n(dst.data(), 4) == 0);
	for (float v : dst) {
		CHECK(v == 0.0f);
	}

	// Partial availability: 1 block pushed, 4 requested -> returns 1 and
	// the available block occupies the NEWEST position of the window;
	// the missing OLDER slots stay silence (0.0).
	MixInputRing partial(2, 64, 8);
	{
		std::vector<float> block(64 * 2, 0.7f);
		partial.push(block.data(), 64);
	}
	std::fill(dst.begin(), dst.end(), -1.0f);
	CHECK(partial.gather_latest_n(dst.data(), 4) == 1);
	for (int s = 0; s < 3 * 64 * 2; ++s) {
		CHECK(dst[s] == 0.0f); // missing older slots are silence
	}
	for (int s = 3 * 64 * 2; s < 4 * 64 * 2; ++s) {
		CHECK(dst[s] == 0.7f); // the one available block, newest position
	}
	std::printf("gather_latest_n done\n");
}

// 2. open() blocksize validation (must be a multiple of libpd_blocksize).
static void test_open_blocksize_validation() {
	NullPort port;
	NativeAudio na;

	CHECK(!na.open(&port, 16, 2, 100, 44100)); // not a multiple of 64
	CHECK(!na.open(&port, 16, 2, 96, 44100));  // 96 % 64 != 0
	CHECK(!na.open(&port, 16, 2, 0, 44100));   // zero
	CHECK(!na.open(&port, 16, 2, 64, 0));      // bad samplerate
	CHECK(!na.open(&port, -1, 2, 64, 44100));  // bad mix_n_in
	CHECK(!na.open(nullptr, 16, 2, 64, 44100));

	// 64 == libpd_blocksize() is a legal stream blocksize (K = 1).
	CHECK(na.open(&port, 16, 2, 64, 44100));
	CHECK(na.is_open());
	CHECK(na.blocksize() == 64);
	CHECK(na.samplerate() == 44100);
	CHECK(port.output_latency_ms() > 0.0);
	CHECK(!na.open(&port, 16, 2, 256, 44100)); // already open
	na.close();
	CHECK(!na.is_open());

	// Other multiples of 64 work too.
	CHECK(na.open(&port, 16, 2, 128, 44100));
	na.close();
	CHECK(na.open(&port, 16, 2, 256, 44100));
	na.close();
	std::printf("open_blocksize_validation done\n");
}

// 3. set_mixer count validation + one-binding contract (invariant #3),
//    and clear_mixer rebind. (Mix patch is the self-contained sine: no
//    rings are registered in this test, so an adc-based mix would render
//    silence and the render-path checks below would be vacuous.)
static void test_set_mixer_validation() {
	write_patch("na_t6_mix.pd", kSinePatch);
	PdInstance mix = init_pd_instance(16, 2, "na_t6_mix.pd");

	NullPort port;
	NativeAudio na;

	// Not open yet: rejected.
	CHECK(!na.set_mixer(mix.pd, 16, 2));
	CHECK(!na.has_mixer());

	CHECK(na.open(&port, 16, 2, 256, 44100));

	// Wrong counts vs the open-time layout: rejected BEFORE binding.
	CHECK(!na.set_mixer(mix.pd, 32, 2));
	CHECK(!na.set_mixer(mix.pd, 16, 4));
	CHECK(!na.set_mixer(nullptr, 16, 2));
	// Nothing bound yet: the drain renders silence.
	sleep_ms(100);
	CHECK(port.last_out_peak() == 0.0f);

	// Correct counts: accepted — the mix render thread now renders the mix
	// and the drain delivers it.
	CHECK(na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());
	sleep_ms(200);
	CHECK(port.frames_rendered() > 5);
	CHECK(port.last_out_peak() > 0.05f); // the sine renders through the mix

	// One binding at a time: rebind rejected while bound.
	CHECK(!na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());

	// clear_mixer() unbinds (stops + joins the mix thread); rebinding the
	// same instance is allowed again.
	na.clear_mixer();
	CHECK(!na.has_mixer());
	CHECK(na.set_mixer(mix.pd, 16, 2));
	CHECK(na.has_mixer());
	sleep_ms(100);
	CHECK(port.frames_rendered() > 10);
	CHECK(port.last_out_peak() > 0.05f); // rendering resumed after rebind

	na.close();
	pd_instance_teardown(mix);
	std::printf("set_mixer_validation done\n");
}

// 4. register_worker_ring divisibility contract (invariant #4).
static void test_register_ring_divisibility() {
	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));

	// 64 divides 256 -> accepted (the Option B production case).
	MixInputRing r64(2, 64, 8);
	na.register_worker_ring(&r64);
	CHECK(na.has_worker_ring(&r64));
	na.unregister_worker_ring(&r64);
	CHECK(!na.has_worker_ring(&r64));

	// 96 does not divide 256 -> rejected.
	MixInputRing r96(2, 96, 8);
	na.register_worker_ring(&r96);
	CHECK(!na.has_worker_ring(&r96));

	// A second, DIFFERENT ring blocksize is rejected (single-size mix).
	MixInputRing r128(2, 128, 8);
	na.register_worker_ring(&r128);
	CHECK(na.has_worker_ring(&r128));
	MixInputRing r64b(2, 64, 8);
	na.register_worker_ring(&r64b);
	CHECK(!na.has_worker_ring(&r64b));

	// 4-channel rings are rejected (2ch layout contract).
	MixInputRing r4(4, 64, 8);
	na.register_worker_ring(&r4);
	CHECK(!na.has_worker_ring(&r4));

	na.close();

	// Pre-open registration validates against the default blocksize (256):
	// 64 divides 256 -> accepted before open().
	NativeAudio na2;
	MixInputRing pre(2, 64, 8);
	na2.register_worker_ring(&pre);
	CHECK(na2.has_worker_ring(&pre));
	na2.close();
	std::printf("register_ring_divisibility done\n");
}

// 5. K-gather end-to-end: 64-frame pushes must render full-rate 256-frame
//    blocks (K = 4). A constant 0.5 fed at worker pace must come out at
//    full level — a gather that delivered fewer than K blocks per callback
//    would mix in stale/zero blocks and drop the average, and a broken
//    divisor would starve the ring entirely.
static void test_k_gather_end_to_end() {
	write_patch("na_t6_kgather.pd", kAdc1Patch);
	PdInstance mix = init_pd_instance(16, 2, "na_t6_kgather.pd");

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));

	MixInputRing ring(2, 64, 8);
	na.register_worker_ring(&ring);
	CHECK(na.has_worker_ring(&ring));
	CHECK(na.set_mixer(mix.pd, 16, 2));

	std::atomic<bool> stop{false};
	std::thread feeder = start_ring_feeder(ring, stop);
	sleep_ms(300);
	stop = true;
	feeder.join();

	const uint64_t blocks = port.frames_rendered();
	CHECK(blocks > 20); // ~50 expected at 256/44100 per 300 ms
	CHECK(port.last_out_peak() > 0.4f); // 0.5 in, *~ 1.0 out

	na.close();
	pd_instance_teardown(mix);
	std::printf("k_gather_end_to_end done (blocks=%llu, peak=%.3f)\n",
			(unsigned long long)blocks, (double)port.last_out_peak());
}

// 6. Full production SYNTH path (Option B): real LibpdWorker renders the
//    sine patch, pushes 64-frame blocks into its own ring; NativeAudio
//    gathers K = 4 blocks per 256-frame callback and renders the mix.
static void test_synth_worker_end_to_end() {
	char tmpl[] = "/tmp/libpd_t6_worker_XXXXXX";
	const int fd = mkstemp(tmpl);
	CHECK(fd >= 0);
	if (fd >= 0) {
		std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
		out.write(kSinePatch, std::strlen(kSinePatch));
		::close(fd);
	}
	const std::string patch_path(tmpl);

	write_patch("na_t6_worker_mix.pd", kAdc1Patch);
	PdInstance mix = init_pd_instance(16, 2, "na_t6_worker_mix.pd");

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));

	// The instance's ring: (2ch, libpd_blocksize(), 8 blocks) — the spec
	// sizing (8 * 64 = 512 frames ~= 11.6 ms of headroom).
	MixInputRing ring(2, libpd_blocksize(), 8);
	na.register_worker_ring(&ring);
	CHECK(na.has_worker_ring(&ring));
	CHECK(na.set_mixer(mix.pd, 16, 2));

	LibpdWorker::Config cfg;
	cfg.instance_id = 9001;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 2;
	cfg.role = LibpdWorker::WorkerRole::SYNTH;
	cfg.worker_ring = &ring; // the worker's only audio path (no sink)
	LibpdWorker worker(cfg);
	worker.start();
	CHECK(init_worker(worker, 0, 2) == 0);
	CHECK(load_patch(worker, patch_path) == 0);
	worker.set_dsp(true);

	sleep_ms(400);

	const uint64_t blocks = port.frames_rendered();
	CHECK(blocks > 20);
	CHECK(port.last_out_peak() > 0.05f); // 0.3-amplitude sine through the mix

	worker.set_dsp(false);
	worker.request_stop();
	worker.join();

	na.close();
	pd_instance_teardown(mix);
	std::remove(patch_path.c_str());
	std::printf("synth_worker_end_to_end done (blocks=%llu, peak=%.3f)\n",
			(unsigned long long)blocks, (double)port.last_out_peak());
}

// 7. Invariant #1 (T4 review, Task 8 rework): the stop path must clear the
//    mixer binding (stop + join the mix render thread) BEFORE the mixer
//    instance is freed. With the drain thread still live, clear_mixer() +
//    free must not touch a dangling pointer; the drain renders silence.
static void test_invariant1_clear_before_teardown() {
	write_patch("na_t6_invar1.pd", kSinePatch);
	PdInstance mix = init_pd_instance(16, 2, "na_t6_invar1.pd");

	NullPort port;
	NativeAudio na;
	CHECK(na.open(&port, 16, 2, 256, 44100));
	CHECK(na.set_mixer(mix.pd, 16, 2));

	// The mix thread is rendering the mix (self-contained sine, no rings).
	sleep_ms(250);
	const uint64_t blocks_before = port.frames_rendered();
	CHECK(blocks_before > 5);
	CHECK(port.last_out_peak() > 0.05f);

	// Stop + join the mix render thread, THEN free the instance while the
	// audio (drain) thread is still live. After the join no thread
	// references the instance; the drain sees the unpublish (nullptr)
	// and renders silence.
	na.clear_mixer();
	CHECK(!na.has_mixer());
	pd_instance_teardown(mix);

	sleep_ms(250);
	CHECK(port.frames_rendered() > blocks_before); // drain still running
	CHECK(port.last_out_peak() == 0.0f);           // silence after unbind

	na.close();
	std::printf("invariant1_clear_before_teardown done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	CHECK(libpd_init() == 0);
	std::printf("server audio tests (M5 Task 6)\n");
	test_gather_latest_n();
	test_open_blocksize_validation();
	test_set_mixer_validation();
	test_register_ring_divisibility();
	test_k_gather_end_to_end();
	test_synth_worker_end_to_end();
	test_invariant1_clear_before_teardown();
	if (failures == 0) {
		std::printf("ALL SERVER AUDIO TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

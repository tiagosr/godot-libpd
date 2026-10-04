// Native worker role tests (M5 Task 5): LibpdWorker's role branch.
//
//   SYNTH   — the a1 render loop, but each rendered block is pushed into
//             the worker's own MixInputRing (the NativeAudio mix-down
//             gathers it in the PortAudio callback) instead of config.sink.
//   MIXER   — control-only: the PortAudio callback renders the mix
//             instance, so the worker never calls libpd_process_float;
//             its RT-sensitive control ops (INIT, LOAD, UNLOAD, teardown)
//             run under config.with_mixer_lock.
//   ANDROID — the existing a1/generator behavior (render -> config.sink),
//             unchanged: this file also pins the ANDROID DEFAULT of a
//             fresh Config so Android + the existing worker tests keep
//             their path.
//
// Same harness as worker_midi_tests.cpp / native_audio_mixdown_tests.cpp:
// standalone main + CHECK, real libpd instances (one per worker, created
// on the worker thread via the INIT command), DrySink as the a1 sink.

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
#include "libpd_worker.h"

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

/** The rendered test patch: [osc~ 200] *~ 0.3 [dac~] — a 2ch sine at
 *  ~0.3 amplitude (same patch as native_audio_mixdown_tests.cpp). */
static const char *const kSinePatch =
		"#N canvas 0 0 200 200 12;\n"
		"#X obj 20 20 osc~ 200;\n"
		"#X obj 20 90 *~ 0.3;\n"
		"#X obj 20 150 dac~;\n"
		"#X connect 0 0 1 0;\n"
		"#X connect 1 0 2 0;\n"
		"#X connect 1 0 2 1;\n";

struct PatchFile {
	std::string path;
	~PatchFile() {
		std::remove(path.c_str());
	}
};

static PatchFile write_temp_patch(const char *p_tag) {
	char tmpl[] = "/tmp/libpd_native_worker_XXXXXX";
	const int fd = mkstemp(tmpl);
	if (fd >= 0) {
		std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
		out.write(kSinePatch, std::strlen(kSinePatch));
		::close(fd);
	}
	(void)p_tag;
	PatchFile p;
	p.path = tmpl;
	return p;
}

// Sentinel distinct from any genuine result code: push_and_wait timed
// out waiting for the worker (hung worker).
static const int k_command_timeout = std::numeric_limits<int>::min();

/** Push a command that reports its result via promise; return the code
 *  (k_command_timeout if the worker never fulfils it within p_timeout_ms). */
static int push_and_wait(LibpdWorker &p_worker, const PdCommand &p_command, int p_timeout_ms = 5000) {
	auto promise = std::make_shared<std::promise<int>>();
	auto future = promise->get_future();
	PdCommand command = p_command;
	// v1 convention: result points to the shared_ptr object itself (the
	// worker reinterprets it as shared_ptr<promise<int>>* and fulfils it).
	command.result = &promise;
	p_worker.push_command(command);
	if (future.wait_for(std::chrono::milliseconds(p_timeout_ms)) != std::future_status::ready) {
		CHECK(false && "command never fulfilled");
		return k_command_timeout;
	}
	return future.get();
}

/** Push INIT (p_n_ins / p_n_out / 44100 Hz). Returns 0 on success,
 *  k_command_timeout if the worker hung, -1 on a genuine init failure. */
static int init_worker(LibpdWorker &p_worker, int p_n_ins, int p_n_out) {
	PdCommand init;
	init.opcode = PdCommand::INIT;
	init.i32 = 44100;
	init.i64 = (int64_t)p_n_ins * 1000 + p_n_out; // packed n_ins*1000+n_out
	return push_and_wait(p_worker, init);
}

/** Push LOAD for p_patch; returns the result code. */
static int load_patch(LibpdWorker &p_worker, const std::string &p_patch) {
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = p_patch;
	return push_and_wait(p_worker, load);
}

/** Max absolute sample in one interleaved block. */
static float block_peak(const float *p_block, int p_n) {
	float peak = 0.0f;
	for (int i = 0; i < p_n; i++) {
		const float a = p_block[i] < 0.0f ? -p_block[i] : p_block[i];
		if (a > peak) {
			peak = a;
		}
	}
	return peak;
}

/**
 * A fresh Config must keep the existing a1/generator behavior: ANDROID
 * role, no ring, no mixer lock, no sink — Android and the existing
 * worker tests must not observe any path change.
 */
static void test_config_default_is_android() {
	LibpdWorker::Config cfg;
	CHECK(cfg.role == LibpdWorker::WorkerRole::ANDROID);
	CHECK(cfg.worker_ring == nullptr);
	CHECK(!cfg.with_mixer_lock);
	CHECK(cfg.sink == nullptr);
	std::printf("config_default_is_android done\n");
}

/**
 * 1: SYNTH worker renders (paced a1 loop) but pushes each block into its
 * MixInputRing — not into config.sink. The ring must be sized to this
 * libpd build's fixed block size (libpd_blocksize() == DEFDACBLKSIZE == 64;
 * see z_libpd.c / s_stuff.h).
 */
static void test_synth_worker_pushes_to_ring() {
	const int bs = 64; // == libpd_blocksize() in this vendored build
	MixInputRing ring(2, bs, 8);

	LibpdWorker::Config cfg;
	cfg.instance_id = 51;
	cfg.role = LibpdWorker::WorkerRole::SYNTH;
	cfg.worker_ring = &ring;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 2;
	cfg.sink = nullptr; // SYNTH must push to the ring, not a sink

	const PatchFile patch = write_temp_patch("synth");

	LibpdWorker worker(cfg);
	worker.start();
	CHECK(worker.pd_instance_ptr() == nullptr); // nothing before INIT

	const int init_result = init_worker(worker, 0, 2);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out (worker hung)\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio for synth ring test (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return;
	}
	CHECK(worker.pd_instance_ptr() != nullptr); // valid after INIT

	CHECK(load_patch(worker, patch.path) == 0);
	worker.set_dsp(true);

	// Kick-driven (M5 T9): the worker blocks on the ring's condition variable
	// and renders exactly one 64-frame block per kick (no self-pacing). Kick,
	// wait for the worker to finish its block, then gather.
	std::atomic<bool> closing{false};
	ring.kick(1);
	ring.wait_done(1, closing);

	std::vector<float> dst(2 * bs, 0.0f);
	CHECK(ring.gather_latest(dst.data(), bs) == bs);
	const float peak = block_peak(dst.data(), 2 * bs);
	CHECK(peak > 0.05f); // osc~ 200 * 0.3 — real audio, not silence
	std::printf("synth_pushes_to_ring done (peak=%.3f)\n", peak);

	worker.request_stop();
	worker.join();
}

/**
 * 2: MIXER worker is control-only. No ring, no rendered audio (the
 * PortAudio callback renders the mix in production) — even with dsp ON,
 * an instance and a loaded patch, nothing reaches the sink. Every
 * RT-sensitive control op runs under with_mixer_lock: INIT, LOAD,
 * UNLOAD, and the teardown free_instance (4 lock-wrapped ops total).
 */
static void test_mixer_worker_is_control_only() {
	DrySink sink; // must remain untouched — MIXER never renders
	std::atomic<int> lock_calls{0};

	LibpdWorker::Config cfg;
	cfg.instance_id = 53;
	cfg.role = LibpdWorker::WorkerRole::MIXER;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 2;
	cfg.sink = &sink;
	cfg.with_mixer_lock = [&lock_calls](std::function<void()> p_f) {
		lock_calls++;
		p_f();
	};

	const PatchFile patch = write_temp_patch("mixer");

	LibpdWorker worker(cfg);
	worker.start();

	const int init_result = init_worker(worker, 0, 2);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out (worker hung)\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio for mixer control test (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return;
	}
	CHECK(lock_calls.load() == 1); // the whole INIT body ran under the lock

	CHECK(load_patch(worker, patch.path) == 0);
	CHECK(lock_calls.load() == 2); // LOAD (openfile) wrapped

	// dsp ON + instance + loaded patch: the render branch must STILL be
	// skipped for MIXER (the callback renders it, not the worker).
	worker.set_dsp(true);
	sleep_ms(100);
	CHECK(sink.blocks_pushed() == 0); // no audio rendered by the worker

	PdCommand unload;
	unload.opcode = PdCommand::UNLOAD;
	CHECK(push_and_wait(worker, unload) == 0);
	CHECK(lock_calls.load() == 3); // UNLOAD (closefile) wrapped

	worker.request_stop();
	worker.join();
	CHECK(lock_calls.load() == 4); // teardown (free_instance) wrapped too
	std::printf("mixer_control_only done (lock_calls=%d, sink_blocks=%llu)\n",
			lock_calls.load(), (unsigned long long)sink.blocks_pushed());
}

/**
 * 3: ANDROID worker still renders (regression): the existing a1
 * DSP+pacing block pushes to config.sink, exactly as before the role
 * branch existed.
 */
static void test_android_worker_still_renders() {
	DrySink sink;

	LibpdWorker::Config cfg;
	cfg.instance_id = 52;
	cfg.role = LibpdWorker::WorkerRole::ANDROID; // explicit (== the default)
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 2;
	cfg.sink = &sink;

	const PatchFile patch = write_temp_patch("android");

	LibpdWorker worker(cfg);
	worker.start();

	const int init_result = init_worker(worker, 0, 2);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out (worker hung)\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio for android regression test (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return;
	}
	CHECK(load_patch(worker, patch.path) == 0);
	worker.set_dsp(true);

	sleep_ms(100); // ~64 paced 64-frame ticks expected
	CHECK(sink.blocks_pushed() > 0);
	std::printf("android_still_renders done (blocks=%llu, peak=%.3f)\n",
			(unsigned long long)sink.blocks_pushed(), sink.peak());

	worker.request_stop();
	worker.join();
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	test_config_default_is_android();
	test_synth_worker_pushes_to_ring();
	test_mixer_worker_is_control_only();
	test_android_worker_still_renders();

	if (failures == 0) {
		std::printf("ALL NATIVE WORKER TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

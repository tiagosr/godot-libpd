// Pd externals tests (pd-externals feature, T1): the vendored cyclone
// + else externals are compiled into the extension and registered
// (register_pdexternals) at process init. Three worker-level checks:
//
//   1. control objects: cyclone [counter] and else [op + 1] driven
//      through a [send] the host subscribes to — deterministic values
//      prove the externals are alive (vanilla pd has neither object);
//   2. non-alphanumeric objects: cyclone [!-] (registered by
//      cyclone_setup itself, not the generated single-lib list);
//   3. audio-rate objects: cyclone [phasor~] + [clip~] + [dac~] on a
//      self-paced worker with a DrySink — the rendered output must be
//      non-silent.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "core/pd_audio_sink.h"
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

struct PatchFile {
	std::string path;
	~PatchFile() {
		std::remove(path.c_str());
	}
};

static PatchFile write_temp_patch(const std::string &p_text) {
	char tmpl[] = "/tmp/libpd_external.XXXXXX";
	const int fd = mkstemp(tmpl);
	if (fd >= 0) {
		std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
		out << p_text;
		::close(fd);
	}
	PatchFile p;
	p.path = tmpl;
	return p;
}

static const int k_command_timeout = std::numeric_limits<int>::min();

static int push_and_wait(LibpdWorker &p_worker, const PdCommand &p_command, int p_timeout_ms = 5000) {
	auto promise = std::make_shared<std::promise<int>>();
	auto future = promise->get_future();
	PdCommand command = p_command;
	command.result = &promise;
	p_worker.push_command(command);
	if (future.wait_for(std::chrono::milliseconds(p_timeout_ms)) != std::future_status::ready) {
		CHECK(false && "command never fulfilled");
		return k_command_timeout;
	}
	return future.get();
}

static int subscribe(LibpdWorker &p_worker, const std::string &p_name) {
	PdCommand cmd;
	cmd.opcode = PdCommand::SUBSCRIBE;
	cmd.path = p_name;
	return push_and_wait(p_worker, cmd);
}

static std::vector<PdEvent> snapshot(std::mutex &p_mutex, std::vector<PdEvent> &p_events) {
	std::lock_guard<std::mutex> lock(p_mutex);
	return p_events;
}

static void wait_until(std::mutex &p_mutex, std::vector<PdEvent> &p_events, int p_expected, int p_timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_timeout_ms);
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(p_mutex);
			if ((int)p_events.size() >= p_expected) {
				return;
			}
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

static void send_message(LibpdWorker &p_worker, const std::string &p_recv, const std::string &p_args) {
	PdCommand msg;
	msg.opcode = PdCommand::MESSAGE;
	msg.path = p_recv;
	msg.args = p_args;
	p_worker.push_command(msg);
}

static void sleep_ms(int p_ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(p_ms));
}

/** Collect FLOAT events for p_name in order. */
static std::vector<float> floats_for(const std::vector<PdEvent> &p_events, const std::string &p_name) {
	std::vector<float> out;
	for (const auto &e : p_events) {
		if (e.type == PdEvent::FLOAT && std::string(e.data) == p_name) {
			out.push_back(e.fval);
		}
	}
	return out;
}

/** Push INIT (p_worker is already constructed with the right Config) and
 *  wait; returns the worker's result code (k_command_timeout on hang). */
static int init_worker(LibpdWorker &p_worker, int p_n_out) {
	PdCommand init;
	init.opcode = PdCommand::INIT;
	init.i32 = 44100;
	init.i64 = (int64_t)0 * 1000 + p_n_out; // packed n_ins*1000+n_out
	return push_and_wait(p_worker, init);
}

/** Build a worker Config (p_n_out outs, event capture, DrySink). */
static LibpdWorker::Config make_config(int p_instance_id, int p_n_out,
		std::mutex &p_mutex, std::vector<PdEvent> &p_events, PdAudioSink *p_sink) {
	LibpdWorker::Config cfg;
	cfg.instance_id = p_instance_id;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = p_n_out;
	cfg.sink = p_sink;
	cfg.on_event = [&p_mutex, &p_events](const PdEvent &p_event) {
		std::lock_guard<std::mutex> lock(p_mutex);
		p_events.push_back(p_event);
	};
	return cfg;
}

// 1) control objects from BOTH externals in one chain: float 5 into
//    cyclone [!- 10 3] (creator default f2=10, creation arg -> left
//    inlet f1; rminus_float then outputs f2 - f1 = 10 - 5 = 5) piped
//    into else [op + 1] -> 6 -> [s host.out]. Neither object exists
//    in vanilla pd.
static void test_control_objects() {
	std::printf("-- control objects (cyclone counter, else op)\n");
	const PatchFile patch = write_temp_patch(
			"#N canvas 0 0 300 200 12;\n"
			"#X obj 10 10 r trig;\n"
			"#X obj 10 50 !- 10 3;\n"
			"#X obj 10 90 op + 1;\n"
			"#X obj 10 130 s host.out;\n"
			"#X connect 0 0 1 0;\n"
			"#X connect 1 0 2 0;\n"
			"#X connect 2 0 3 0;\n");

	std::mutex ev_mutex;
	std::vector<PdEvent> events;
	DrySink sink;

	LibpdWorker worker(make_config(21, 0, ev_mutex, events, &sink));
	worker.start();
	const int init_result = init_worker(worker, 0);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return;
	}

	CHECK(subscribe(worker, "host.out") == 0);
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	CHECK(push_and_wait(worker, load) == 0);
	sleep_ms(100);

	send_message(worker, "trig", "5");
	wait_until(ev_mutex, events, 1, 4000);
	const std::vector<float> vals = floats_for(snapshot(ev_mutex, events), "host.out");
	CHECK(vals.size() == 1);
	if (vals.size() == 1) {
		CHECK(std::fabsf(vals[0] - 6.0f) < 1e-6f); // 10 - 5 + 1
	}

	worker.request_stop();
	worker.join();
}

// 2) non-alphanumeric: cyclone [!- 10] bangs f2 - f1 = 10 - 0 = 10
//    (cyclone_setup registers these directly, not via the generated
//    single-lib list).
static void test_nonalphanumeric() {
	std::printf("-- non-alphanumeric (cyclone !-)\n");
	const PatchFile patch = write_temp_patch(
			"#N canvas 0 0 300 200 12;\n"
			"#X obj 10 10 r trig;\n"
			"#X obj 10 50 !- 10;\n"
			"#X obj 10 90 s host.out;\n"
			"#X connect 0 0 1 0;\n"
			"#X connect 1 0 2 0;\n");

	std::mutex ev_mutex;
	std::vector<PdEvent> events;
	DrySink sink;

	LibpdWorker worker(make_config(22, 0, ev_mutex, events, &sink));
	worker.start();
	const int init_result = init_worker(worker, 0);
	if (init_result == k_command_timeout || init_result != 0) {
		worker.request_stop();
		worker.join();
		return;
	}

	CHECK(subscribe(worker, "host.out") == 0);
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	CHECK(push_and_wait(worker, load) == 0);
	sleep_ms(100);

	send_message(worker, "trig", "");
	wait_until(ev_mutex, events, 1, 4000);
	const std::vector<float> vals = floats_for(snapshot(ev_mutex, events), "host.out");
	CHECK(vals.size() == 1);
	if (vals.size() == 1) {
		CHECK(std::fabsf(vals[0] - 10.0f) < 1e-6f);
	}

	worker.request_stop();
	worker.join();
}

// 3) audio-rate: cyclone [phasor~ 440] -> [clip~ -1 1] -> [dac~];
//    self-paced worker renders blocks into the DrySink; the output
//    must be non-silent.
static void test_dsp_objects() {
	std::printf("-- audio-rate (cyclone phasor~ + clip~)\n");
	const PatchFile patch = write_temp_patch(
			"#N canvas 0 0 300 200 12;\n"
			"#X obj 10 10 phasor~ 440;\n"
			"#X obj 10 50 clip~ -1 1;\n"
			"#X obj 10 90 dac~;\n"
			"#X connect 0 0 1 0;\n"
			"#X connect 1 0 2 0;\n"
			"#X connect 1 0 2 1;\n");

	std::mutex ev_mutex;
	std::vector<PdEvent> events;
	DrySink sink;

	LibpdWorker worker(make_config(23, 2, ev_mutex, events, &sink));
	worker.start();
	const int init_result = init_worker(worker, 2);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return;
	}

	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	CHECK(push_and_wait(worker, load) == 0);
	worker.set_dsp(true);
	sleep_ms(300); // ~14 blocks at 64-frame / 44.1 kHz

	const uint64_t blocks = sink.blocks_pushed();
	const float peak = sink.peak();
	std::printf("   blocks=%llu peak=%.3f\n", (unsigned long long)blocks, peak);
	CHECK(blocks > 0);
	CHECK(peak > 0.05f); // phasor~ 440 — real audio, not silence

	worker.request_stop();
	worker.join();
}


// 4) in-house external tjcount (externals/tjcount): up-only 0..3 counter.
//    Bangs give 1,2,3,wrap->0; the wrap bang (right outlet -> host.wrap)
//    must arrive BEFORE the wrapped count (left outlet -> host.count).
//    Out-of-range floats clamp; "set" is silent; an invalid "min 5"
//    (>= max) is rejected without changing the range.
static void test_tjcount() {
	std::printf("-- tjcount (in-house external)\n");
	const PatchFile patch = write_temp_patch(
			"#N canvas 0 0 400 300 12;\n"
			"#X obj 10 10 r trig;\n"
			"#X obj 10 40 r setin;\n"
			"#X obj 10 70 tjcount 0 3;\n"
			"#X obj 10 110 s host.count;\n"
			"#X obj 10 140 s host.wrap;\n"
			"#X connect 0 0 2 0;\n"
			"#X connect 1 0 2 0;\n"
			"#X connect 2 0 3 0;\n"
			"#X connect 2 1 4 0;\n");

	std::mutex ev_mutex;
	std::vector<PdEvent> events;
	DrySink sink;

	LibpdWorker worker(make_config(24, 0, ev_mutex, events, &sink));
	worker.start();
	const int init_result = init_worker(worker, 0);
	if (init_result == k_command_timeout || init_result != 0) {
		worker.request_stop();
		worker.join();
		return;
	}

	CHECK(subscribe(worker, "host.count") == 0);
	CHECK(subscribe(worker, "host.wrap") == 0);
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	CHECK(push_and_wait(worker, load) == 0);
	sleep_ms(100);

	// wrap: 1, 2, 3, 0 (bang on the 0)
	for (int i = 0; i < 4; i++)
		send_message(worker, "trig", "");
	sleep_ms(200);
	// 1 (no wrap), then clamp 99 -> 3, then set 1 (silent) + bang -> 2,
	// then rejected min + bang -> 3 (no wrap: range unchanged).
	send_message(worker, "trig", "");
	send_message(worker, "trig", "99");
	send_message(worker, "setin", "set 1");
	send_message(worker, "trig", "");
	send_message(worker, "setin", "min 5");
	send_message(worker, "trig", "");
	sleep_ms(300);

	const std::vector<PdEvent> snap = snapshot(ev_mutex, events);
	const std::vector<float> vals = floats_for(snap, "host.count");
	std::printf("   count:");
	for (float v : vals)
		std::printf(" %g", v);
	std::printf("\n");
	int wrap_bangs = 0;
	int wrap_idx = -1, wrapped_zero_idx = -1;
	for (size_t i = 0; i < snap.size(); i++) {
		const PdEvent &e = snap[i];
		if (e.type == PdEvent::BANG && std::string(e.data) == "host.wrap") {
			wrap_bangs++;
			if (wrap_idx < 0) {
				wrap_idx = (int)i;
			}
		}
		if (e.type == PdEvent::FLOAT && std::string(e.data) == "host.count"
				&& e.fval == 0.0f) {
			wrapped_zero_idx = (int)i; // last one is the wrap
		}
	}
	std::vector<float> expected = { 1.f, 2.f, 3.f, 0.f, 1.f, 3.f, 2.f, 3.f };
	CHECK(vals.size() == expected.size());
	if (vals.size() == expected.size()) {
		for (size_t i = 0; i < expected.size(); i++)
			CHECK(std::fabsf(vals[i] - expected[i]) < 1e-6f);
	}
	CHECK(wrap_bangs == 1);
	CHECK(wrap_idx >= 0 && wrap_idx < wrapped_zero_idx); // bang before count

	worker.request_stop();
	worker.join();
}

static void test_tjlistfind() {
	std::printf("-- tjlistfind (in-house external)\n");
	const PatchFile patch = write_temp_patch(
			"#N canvas 0 0 400 300 12;\n"
			"#X obj 10 10 r l;\n"
			"#X obj 10 40 r cmd;\n"
			"#X obj 10 70 tjlistfind 2.0;\n"
			"#X obj 10 100 tjlistfind 0.1 0.001;\n"
			"#X obj 10 130 s host.idxA;\n"
			"#X obj 10 160 s host.idxB;\n"
			"#X connect 0 0 2 0;\n"
			"#X connect 1 0 2 0;\n"
			"#X connect 2 0 4 0;\n"
			"#X connect 0 0 3 0;\n"
			"#X connect 3 0 5 0;\n");

	std::mutex ev_mutex;
	std::vector<PdEvent> events;
	DrySink sink;

	LibpdWorker worker(make_config(24, 0, ev_mutex, events, &sink));
	worker.start();
	const int init_result = init_worker(worker, 0);
	if (init_result == k_command_timeout || init_result != 0) {
		worker.request_stop();
		worker.join();
		return;
	}

	CHECK(subscribe(worker, "host.idxA") == 0);
	CHECK(subscribe(worker, "host.idxB") == 0);
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	CHECK(push_and_wait(worker, load) == 0);
	sleep_ms(100);

	// A: target 2.0 exact.  B: target 0.1, tolerance 0.001.
	// 1. list 1 2 3        -> A:2 (match at 1-indexed 2), B:0
	// 2. list 9 foo 2 7    -> A:3 (symbol skipped), B:0
	// 3. float 2           -> A:1 (single float = 1-element list), B:0
	// 4. float 7           -> A:0
	// 5. list 5.5 0.1005 3 -> A:0, B:2 (|0.1005-0.1| <= 0.001)
	// 6. find 0.1005, list 5 0.1005 -> A:2, B:2 (method retargets)
	// 7. find 2.0, tolerance 0.05, list 2.03 -> A:1 (within new tolerance), B:0
	send_message(worker, "l", "1 2 3");
	send_message(worker, "l", "9 foo 2 7");
	send_message(worker, "l", "2");
	send_message(worker, "l", "7");
	send_message(worker, "l", "5.5 0.1005 3");
	send_message(worker, "cmd", "find 0.1005");
	send_message(worker, "l", "5 0.1005");
	send_message(worker, "cmd", "find 2.0");
	send_message(worker, "cmd", "tolerance 0.05");
	send_message(worker, "l", "2.03");
	sleep_ms(300);

	const std::vector<PdEvent> snap = snapshot(ev_mutex, events);
	const std::vector<float> idxA = floats_for(snap, "host.idxA");
	const std::vector<float> idxB = floats_for(snap, "host.idxB");
	std::printf("   idxA:");
	for (float v : idxA)
		std::printf(" %g", v);
	std::printf("\n   idxB:");
	for (float v : idxB)
		std::printf(" %g", v);
	std::printf("\n");
	std::vector<float> expA = { 2.f, 3.f, 1.f, 0.f, 0.f, 2.f, 1.f };
	std::vector<float> expB = { 0.f, 0.f, 0.f, 0.f, 2.f, 2.f, 0.f };
	CHECK(idxA.size() == expA.size());
	CHECK(idxB.size() == expB.size());
	if (idxA.size() == expA.size()) {
		for (size_t i = 0; i < expA.size(); i++)
			CHECK(std::fabsf(idxA[i] - expA[i]) < 1e-6f);
	}
	if (idxB.size() == expB.size()) {
		for (size_t i = 0; i < expB.size(); i++)
			CHECK(std::fabsf(idxB[i] - expB[i]) < 1e-6f);
	}

	worker.request_stop();
	worker.join();
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_control_objects();
	test_nonalphanumeric();
	test_dsp_objects();
	test_tjcount();
	test_tjlistfind();
	std::printf("externals done\n");
	std::printf("%d FAILURES\n", failures);
	return failures == 0 ? 0 : 1;
}

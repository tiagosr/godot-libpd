// Abstraction-args tests (abstraction-args feature, T1): one real pd
// instance, no DSP. The worker's LOAD command with non-empty args must
// generate a temp top-level loader canvas containing [name args...] and
// load it, so pd's standard abstraction machinery substitutes $1/$2/...
// inside the loaded abstraction.
//
// Proving substitution: the abstraction's [receive] name embeds the arg
// (r synth-$1). The host subscribes a DIFFERENT name (host.out) that the
// patch [send]s to — no name stealing (the host never binds the
// substituted name itself). A bang to synth-42 only reaches the host if
// the patch really created [r synth-42].

#include <chrono>
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

static std::string make_temp_dir() {
	char tmpl[] = "/tmp/libpd_abstr.XXXXXX";
	if (mkdtemp(tmpl) == nullptr) {
		return "";
	}
	return tmpl;
}

static bool write_file(const std::string &p_path, const std::string &p_content) {
	std::ofstream out(p_path, std::ios::binary | std::ios::trunc);
	if (!out) {
		return false;
	}
	out << p_content;
	return static_cast<bool>(out);
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

static int load_abstraction(LibpdWorker &p_worker, const std::string &p_name, const std::string &p_args) {
	PdCommand cmd;
	cmd.opcode = PdCommand::LOAD;
	cmd.path = p_name;
	cmd.args = p_args;
	return push_and_wait(p_worker, cmd);
}

// Count BANG events addressed to p_name in p_events.
static int count_bangs(const std::vector<PdEvent> &p_events, const std::string &p_name) {
	int n = 0;
	for (const auto &e : p_events) {
		if (e.type == PdEvent::BANG && std::string(e.data) == p_name) {
			n++;
		}
	}
	return n;
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const std::string dir = make_temp_dir();
	if (dir.empty()) {
		std::printf("FAIL: mkdtemp\n%d FAILURES\n", failures);
		std::exit(1);
	}

	// synth.pd: [r synth-$1] -> [s host.out]
	// multi.pd: [r m-$1-$2] -> [s host.out]
	// plain.pd: [r plain]   -> [s host.out]  (no-args regression)
	if (!write_file(dir + "/synth.pd",
				"#N canvas 0 0 200 200 12;\n"
				"#X obj 10 10 r synth-$1;\n"
				"#X obj 10 50 s host.out;\n"
				"#X connect 0 0 1 0;\n")) {
		std::printf("FAIL: write synth.pd\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (!write_file(dir + "/multi.pd",
				"#N canvas 0 0 200 200 12;\n"
				"#X obj 10 10 r m-$1-$2;\n"
				"#X obj 10 50 s host.out;\n"
				"#X connect 0 0 1 0;\n")) {
		std::printf("FAIL: write multi.pd\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (!write_file(dir + "/plain.pd",
				"#N canvas 0 0 200 200 12;\n"
				"#X obj 10 10 r plain;\n"
				"#X obj 10 50 s host.out;\n"
				"#X connect 0 0 1 0;\n")) {
		std::printf("FAIL: write plain.pd\n%d FAILURES\n", failures);
		std::exit(1);
	}

	std::mutex ev_mutex;
	std::vector<PdEvent> events;

	LibpdWorker::Config cfg;
	cfg.instance_id = 11;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 0;
	cfg.sink = nullptr;
	cfg.on_event = [&ev_mutex, &events](const PdEvent &p_event) {
		std::lock_guard<std::mutex> lock(ev_mutex);
		events.push_back(p_event);
	};

	LibpdWorker worker(cfg);
	worker.start();

	PdCommand init;
	init.opcode = PdCommand::INIT;
	init.i32 = 44100;
	init.i64 = 0;
	const int init_result = push_and_wait(worker, init);
	if (init_result == k_command_timeout) {
		std::printf("FAIL: INIT timed out (worker hung)\n%d FAILURES\n", failures);
		std::exit(1);
	}
	if (init_result != 0) {
		std::printf("SKIP: no audio for abstraction test (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return 0;
	}

	// The patch [send]s to host.out; bind it BEFORE loading so the
	// instantiation (which may loadbang) has a listener ready.
	CHECK(subscribe(worker, "host.out") == 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// 1) Single-arg abstraction: $1 = 42.
	{
		const int rc = load_abstraction(worker, dir + "/synth.pd", "42");
		if (rc == k_command_timeout) {
			std::printf("FAIL: LOAD timed out\n%d FAILURES\n", failures);
			std::exit(1);
		}
		CHECK(rc == 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		send_message(worker, "synth-42", "");
		const auto before = snapshot(ev_mutex, events).size();
		wait_until(ev_mutex, events, (int)before + 1, 4000);
		CHECK(count_bangs(snapshot(ev_mutex, events), "host.out") == 1);
	}

	// 2) Two-arg abstraction: $1=7, $2=8  ->  m-7-8.
	{
		const int rc = load_abstraction(worker, dir + "/multi.pd", "7 8");
		CHECK(rc == 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		send_message(worker, "m-7-8", "");
		const auto before = snapshot(ev_mutex, events).size();
		wait_until(ev_mutex, events, (int)before + 1, 4000);
		CHECK(count_bangs(snapshot(ev_mutex, events), "host.out") == 2);
	}

	// 3) Regression: a plain (no-args) LOAD still loads a patch directly.
	{
		PdCommand load;
		load.opcode = PdCommand::LOAD;
		load.path = dir + "/plain.pd"; // no args
		const int rc = push_and_wait(worker, load);
		CHECK(rc == 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		send_message(worker, "plain", "");
		const auto before = snapshot(ev_mutex, events).size();
		wait_until(ev_mutex, events, (int)before + 1, 4000);
		CHECK(count_bangs(snapshot(ev_mutex, events), "host.out") == 3);
	}

	worker.request_stop();
	worker.join();

	// Clean up the temp dir contents.
	std::remove((dir + "/synth.pd").c_str());
	std::remove((dir + "/multi.pd").c_str());
	std::remove((dir + "/plain.pd").c_str());
	::rmdir(dir.c_str());

	std::printf("abstraction done\n");
	std::printf("%d FAILURES\n", failures);
	return failures == 0 ? 0 : 1;
}

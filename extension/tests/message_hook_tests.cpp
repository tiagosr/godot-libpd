// Message hook tests (receive-hook feature, T1): one real pd instance, no
// DSP. Receive hooks fire ONLY for host-bound receivers (libpd_bind's
// virtual [r name] — see x_libpdreceive.c), so the test subscribes names
// (SUBSCRIBE) and then:
//   1. sends bang/float/symbol/list directly to the bound names
//      (MESSAGE opcode — fire-and-forget, no result promise), and
//   2. drives a real patch path: [r trig] -> [send test.send] with
//      "test.send" bound, to prove genuine [send] delivery.
// Also covers: receiver name truncation, unsubscribe, duplicate subscribe.

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

// 68-byte bound name ("r." + 66 x 'a') > the 64-byte data field.
static const char *k_long_name =
		"aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" // 20
		"aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" // 40
		"aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" "aa" // 60
		"aa" "aa" "aa"; // 66

struct PatchFile {
	std::string path;
	~PatchFile() {
		std::remove(path.c_str());
	}
};

// [r trig] -> [send test.send]: a genuine patch [send] to a host-bound name.
static PatchFile write_temp_patch() {
	char tmpl[] = "/tmp/libpd_msghook.XXXXXX";
	const int fd = mkstemp(tmpl);
	if (fd >= 0) {
		std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
		out << "#N canvas 0 0 400 200 12;\n"
			<< "#X obj 10 10 r trig;\n"
			<< "#X obj 10 50 send test.send;\n"
			<< "#X connect 0 0 1 0;\n";
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

static void unsubscribe(LibpdWorker &p_worker, const std::string &p_name) {
	PdCommand cmd;
	cmd.opcode = PdCommand::UNSUBSCRIBE;
	cmd.path = p_name;
	(void)push_and_wait(p_worker, cmd);
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
	p_worker.push_command(msg); // fire-and-forget (MESSAGE has no result promise)
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const PatchFile patch = write_temp_patch();

	std::mutex ev_mutex;
	std::vector<PdEvent> events;

	LibpdWorker::Config cfg;
	cfg.instance_id = 7;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 0;
	cfg.sink = nullptr; // message path only
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
		std::printf("SKIP: no audio for message hook test (init result %d)\n", init_result);
		worker.request_stop();
		worker.join();
		return 0;
	}

	// Subscribe the names the test sends to.
	CHECK(subscribe(worker, "test.bang") == 0);
	CHECK(subscribe(worker, "test.float") == 0);
	CHECK(subscribe(worker, "test.symbol") == 0);
	CHECK(subscribe(worker, "test.list") == 0);
	CHECK(subscribe(worker, "test.send") == 0); // for the patch [send] path
	// Duplicate subscribe must fail.
	CHECK(subscribe(worker, "test.bang") == -1);
	// Long name for truncation.
	CHECK(subscribe(worker, std::string("r.") + k_long_name) == 0);
	// Empty name must fail.
	CHECK(subscribe(worker, "") == -1);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// Bang.
	{
		send_message(worker, "test.bang", "");
		wait_until(ev_mutex, events, 1, 4000);
		int bangs = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::BANG && std::string(e.data) == "test.bang") {
				bangs++;
			}
		}
		CHECK(bangs == 1);
	}

	// Float ("0.618" parses to the same float on both sides via strtof).
	{
		send_message(worker, "test.float", "0.618");
		wait_until(ev_mutex, events, 2, 4000);
		int floats = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::FLOAT && std::string(e.data) == "test.float") {
				floats++;
				CHECK(e.fval == 0.618f);
			}
		}
		CHECK(floats == 1);
	}

	// Symbol.
	{
		send_message(worker, "test.symbol", "attack");
		wait_until(ev_mutex, events, 3, 4000);
		int syms = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::SYMBOL && std::string(e.data) == "test.symbol") {
				syms++;
				CHECK(std::string(e.sval) == "attack");
			}
		}
		CHECK(syms == 1);
	}

	// List: 3 items (float, symbol, float).
	{
		send_message(worker, "test.list", "1.5 attack 2.0");
		wait_until(ev_mutex, events, 4, 4000);
		int lists = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::LIST && std::string(e.data) == "test.list") {
				lists++;
				CHECK(e.n_items == 3);
				CHECK(e.list_is_symbol[0] == 0);
				CHECK(e.list_floats[0] == 1.5f);
				CHECK(e.list_is_symbol[1] == 1);
				CHECK(std::string(e.list_syms[1]) == "attack");
				CHECK(e.list_is_symbol[2] == 0);
				CHECK(e.list_floats[2] == 2.0f);
			}
		}
		CHECK(lists == 1);
	}

	// Long bound name: truncated to 63 bytes, NUL-terminated, prefix.
	{
		send_message(worker, std::string("r.") + k_long_name, "");
		wait_until(ev_mutex, events, 5, 4000);
		int longs = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::BANG && std::string(e.data).size() > 10) {
				longs++;
				CHECK(std::strlen(e.data) <= 63);
				CHECK(e.data[63] == '\0');
				CHECK(std::strncmp(e.data, "r.", 2) == 0);
				CHECK(std::string(e.data).compare(0, 63, std::string("r.") + k_long_name, 0, 63) == 0);
				CHECK(e.sval[0] == '\0');
			}
		}
		CHECK(longs == 1);
	}

	// Real patch [send] path: load, then bang "trig".
	{
		PdCommand load;
		load.opcode = PdCommand::LOAD;
		load.path = patch.path;
		const int load_result = push_and_wait(worker, load);
		if (load_result != k_command_timeout && load_result != 0) {
			std::printf("FAIL: patch load failed (%s)\n", patch.path.c_str());
			failures++;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		send_message(worker, "trig", "");
		const auto before = snapshot(ev_mutex, events).size();
		wait_until(ev_mutex, events, (int)before + 1, 4000);
		int sends = 0;
		for (const auto &e : snapshot(ev_mutex, events)) {
			if (e.type == PdEvent::BANG && std::string(e.data) == "test.send") {
				sends++;
			}
		}
		CHECK(sends == 1);
	}

	// Unsubscribe stops delivery (idempotent + effective).
	{
		unsubscribe(worker, "test.bang");
		unsubscribe(worker, "test.bang"); // idempotent no-op
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto before = snapshot(ev_mutex, events).size();
		send_message(worker, "test.bang", "");
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		auto after = snapshot(ev_mutex, events);
		int new_bangs = 0;
		for (size_t i = before; i < after.size(); i++) {
			if (after[i].type == PdEvent::BANG && std::string(after[i].data) == "test.bang") {
				new_bangs++;
			}
		}
		CHECK(new_bangs == 0);
		// Re-subscribing after unsubscribe works again.
		CHECK(subscribe(worker, "test.bang") == 0);
		send_message(worker, "test.bang", "");
		const auto before2 = snapshot(ev_mutex, events).size();
		wait_until(ev_mutex, events, (int)before2 + 1, 4000);
		int resub_bangs = 0;
		for (size_t i = before2; i < snapshot(ev_mutex, events).size(); i++) {
			if (snapshot(ev_mutex, events)[i].type == PdEvent::BANG && std::string(snapshot(ev_mutex, events)[i].data) == "test.bang") {
				resub_bangs++;
			}
		}
		CHECK(resub_bangs == 1);
	}

	worker.request_stop();
	worker.join();
	std::printf("message_hook done\n");
	std::printf("%d FAILURES\n", failures);
	return failures == 0 ? 0 : 1;
}

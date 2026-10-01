// Worker MIDI tests (Task 2): one real pd instance, no DSP — drives the
// per-instance worker end-to-end through its command queue (INIT/LOAD/
// MIDI_* opcodes execute on the worker thread, which owns the pd
// instance; pd_this is thread-local, so the worker thread must create
// the instance). Output hooks fill worker.midi_out; later tasks drain
// it for PortMIDI output. Idle-instance delivery (spec §4) is the point.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
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

// Test patch: [notein 1] -> [noteout 1] (pitch + velocity wired),
// [ctlin 7 4] -> [ctlout 7 4] (1-based channel 4 = 0-based ch 3), and
// [midiin] -> [print] to prove raw bytes reach pd. Raw bytes do NOT
// cross-feed [notein] (plan Global Constraints: separate input paths).
static const char *k_test_patch =
		"#N canvas 0 0 300 200 12;\n"
		"#X obj 10 10 notein 1;\n"
		"#X obj 10 50 noteout 1;\n"
		"#X obj 100 10 ctlin 7 4;\n"
		"#X obj 100 50 ctlout 7 4;\n"
		"#X obj 200 10 midiin;\n"
		"#X obj 200 50 print midiin_chk;\n"
		"#X connect 0 0 1 0;\n"
		"#X connect 0 1 1 1;\n"
		"#X connect 2 0 3 0;\n"
		"#X connect 4 0 5 0;\n";

struct PatchFile {
	std::string path;
	~PatchFile() {
		std::remove(path.c_str());
	}
};

static PatchFile write_temp_patch() {
	char tmpl[] = "/tmp/libpd_worker_midi.XXXXXX";
	const int fd = mkstemp(tmpl);
	if (fd >= 0) {
		std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
		out.write(k_test_patch, std::strlen(k_test_patch));
		::close(fd);
	}
	PatchFile p;
	p.path = tmpl;
	return p;
}

/** Push a command that reports its result via promise; return the code. */
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
		return -1;
	}
	return future.get();
}

/** Drain the worker output queue into r_msgs (FIFO order). */
static int drain(LibpdWorker &p_worker, std::vector<MidiOutMsg> &r_msgs) {
	return p_worker.midi_out.pop_all(r_msgs);
}

/** Wait until the queue holds >= p_expected messages (p_timeout_ms budget). */
static bool wait_for_msgs(LibpdWorker &p_worker, int p_expected, int p_timeout_ms,
		std::vector<MidiOutMsg> &r_msgs) {
	const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(p_timeout_ms);
	for (;;) {
		if (drain(p_worker, r_msgs) >= p_expected) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

static void test_midi_output_queue_overflow() {
	// Pure queue behavior (spec §4 drop-oldest); no pd involved.
	MidiOutputQueue q;
	const int n = MidiOutputQueue::CAPACITY + 1;
	for (int i = 0; i < n; i++) {
		MidiOutMsg m;
		m.kind = MidiOutMsg::NOTE;
		m.d1 = (uint8_t)(i >> 8);
		m.d2 = (uint8_t)(i & 0xFF);
		q.push(m);
	}
	CHECK(q.dropped() == 1);
	std::vector<MidiOutMsg> out;
	CHECK(q.pop_all(out) == MidiOutputQueue::CAPACITY);
	CHECK(out.front().d1 == 0 && out.front().d2 == 1); // event #0 dropped, #1 kept
	CHECK(out.back().d1 == 16 && out.back().d2 == 0); // newest last (i = n-1 = 4096)
	std::printf("midi_output_queue_overflow done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const PatchFile patch = write_temp_patch();

	std::mutex print_mutex;
	std::vector<std::string> prints;

	LibpdWorker::Config cfg;
	cfg.instance_id = 42;
	cfg.samplerate = 44100;
	cfg.n_ins = 0;
	cfg.n_out = 0;
	cfg.sink = nullptr; // no audio I/O; the test exercises the message path only
	cfg.on_event = [&print_mutex, &prints](const PdEvent &p_event) {
		if (p_event.type != PdEvent::PRINT) {
			return;
		}
		std::lock_guard<std::mutex> lock(print_mutex);
		prints.push_back(std::string(p_event.data));
	};

	LibpdWorker worker(cfg);
	worker.start();

	// INIT: worker creates the pd instance on its own thread
	// (libpd_init -> libpd_new_instance -> libpd_set_instance) and
	// initializes audio (0 in / 0 out / 44100 Hz). Skip-don't-fail on failure.
	PdCommand init;
	init.opcode = PdCommand::INIT;
	init.i32 = 44100;
	init.i64 = 0; // n_ins * 1000 + n_out
	if (push_and_wait(worker, init) != 0) {
		std::printf("SKIP: no audio for worker midi test\n");
		worker.request_stop();
		worker.join();
		return 0;
	}

	// LOAD the temp patch.
	PdCommand load;
	load.opcode = PdCommand::LOAD;
	load.path = patch.path;
	if (push_and_wait(worker, load) != 0) {
		std::printf("FAIL: patch load failed (%s)\n", patch.path.c_str());
		failures++;
		worker.request_stop();
		worker.join();
		std::printf("%d FAILURES\n", failures);
		return 1;
	}

	// Case 1: MIDI_NOTE (ch0, 60, 100) -> [noteout] -> noteonhook
	// -> NOTE{0,60,100} in the queue (in addition to the PdEvent ring).
	{
		PdCommand note;
		note.opcode = PdCommand::MIDI_NOTE;
		note.i32 = 0;
		note.i64 = 60 * 256 + 100;
		worker.push_command(note);
		std::vector<MidiOutMsg> msgs;
		CHECK(wait_for_msgs(worker, 1, 5000, msgs));
		CHECK((int)msgs.size() == 1);
		if (msgs.size() == 1) {
			CHECK(msgs[0].kind == MidiOutMsg::NOTE);
			CHECK(msgs[0].channel == 0);
			CHECK(msgs[0].d1 == 60);
			CHECK(msgs[0].d2 == 100);
		}
		std::printf("case midi_note done\n");
	}

	// Case 2: MIDI_CC (ch3, ctrl7, 127) -> [ctlout 7 4] -> CC{3,7,127}.
	{
		PdCommand cc;
		cc.opcode = PdCommand::MIDI_CC;
		cc.i32 = 3;
		cc.i64 = 7 * 256 + 127;
		worker.push_command(cc);
		std::vector<MidiOutMsg> msgs;
		CHECK(wait_for_msgs(worker, 1, 5000, msgs));
		CHECK((int)msgs.size() == 1);
		if (msgs.size() == 1) {
			CHECK(msgs[0].kind == MidiOutMsg::CC);
			CHECK(msgs[0].channel == 3);
			CHECK(msgs[0].d1 == 7);
			CHECK(msgs[0].d2 == 127);
		}
		std::printf("case midi_cc done\n");
	}

	// Case 3: MIDI_BYTE 0x90 / 0x3C / 0x64. Raw bytes reach pd's [midiin]
	// only (no cross-feed to [notein] in this build) -> midi_out must stay
	// unchanged; [print midiin_chk] proves the bytes actually arrived.
	{
		const uint8_t raw_bytes[] = {0x90, 0x3C, 0x64};
		for (uint8_t b : raw_bytes) {
			PdCommand byte_cmd;
			byte_cmd.opcode = PdCommand::MIDI_BYTE;
			byte_cmd.i64 = b;
			worker.push_command(byte_cmd);
		}
		const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::milliseconds(5000);
		// Each [print] of one float yields two print events (observed on
		// first run): "midiin_chk: <value>" followed by "\n".
		size_t n_prints = 0;
		while (n_prints < 6) {
			{
				std::lock_guard<std::mutex> lock(print_mutex);
				n_prints = prints.size();
			}
			if (n_prints >= 6 || std::chrono::steady_clock::now() >= deadline) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		std::lock_guard<std::mutex> lock(print_mutex);
		for (size_t i = 0; i < n_prints; i++) {
			std::printf("midiin print[%zu] = '%s'\n", i, prints[i].c_str());
		}
		CHECK(n_prints == 6);
		if (n_prints >= 6) {
			CHECK(prints[0] == "midiin_chk: 144");
			CHECK(prints[1] == "\n");
			CHECK(prints[2] == "midiin_chk: 60");
			CHECK(prints[3] == "\n");
			CHECK(prints[4] == "midiin_chk: 100");
			CHECK(prints[5] == "\n");
		}
		std::vector<MidiOutMsg> msgs;
		CHECK(drain(worker, msgs) == 0); // queue unchanged (no [noteout] activity)
		std::printf("case midi_byte done\n");
	}

	// Case 4: MIDI_SYSEX (F0 7E 7F 09 F7) -> no crash, queue unchanged
	// (the patch has no [sysexin]; libpd has no sysex output hook either).
	{
		PdCommand sysex;
		sysex.opcode = PdCommand::MIDI_SYSEX;
		sysex.midi_len = 5;
		sysex.midi[0] = 0xF0;
		sysex.midi[1] = 0x7E;
		sysex.midi[2] = 0x7F;
		sysex.midi[3] = 0x09;
		sysex.midi[4] = 0xF7;
		worker.push_command(sysex);
		// Settle: the worker's idle loop picks up commands within ~1 ms.
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::vector<MidiOutMsg> msgs;
		CHECK(drain(worker, msgs) == 0);
		std::printf("case midi_sysex done\n");
	}

	// Case 5: bounded queue overflow (drop-oldest, newest last).
	test_midi_output_queue_overflow();

	// Teardown on the worker thread (spec §5).
	worker.request_stop();
	worker.join();

	if (failures == 0) {
		std::printf("ALL WORKER MIDI TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

// Multi-instance MIDI regression tests (whole-branch review P1-1,
// investigated and disproven — see extension/README.md "Multi-instance
// MIDI hook attribution"):
//
//   1. Hook attribution: two live pd instances on two worker threads.
//      A note driven through one instance must produce a hook event in
//      that instance's worker ONLY, in both directions. Under the
//      suspected bug (a single process-wide instancedata where the
//      last-initialized worker owns every hook), a note through A would
//      land in B's queue.
//   2. Routed fan-out: synthetic port loopback A-out -> port P -> B-in,
//      simulated in-process with the real MidiRoutingTable lookups and a
//      local mirror of MidiRouter::build_command — no PortMIDI I/O,
//      no PortMIDI handles. A's [noteout] hook event is popped from
//      A's midi_out queue (as the router's output stage does), re-built
//      into a PdCommand, and delivered to every instance routed from
//      port P. B's [noteout] must then fire — proof that the routed
//      command reached B's [notein] on B's own instance.
//
// Real LibpdWorkers, in-process, no audio output (n_out = 0, no sink).
// The first INIT initializes the global pd library; each worker then
// creates its own pd instance on its own thread (pd_this is thread-
// local; libpd is built PD_MULTI=ON, forced in extension/CMakeLists.txt).

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

#include "core/midi_routing_table.h"
#include "core/pd_midi_framer.h"
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

// Test patch (same for both workers): [notein 0] -> [noteout 1].
// [notein 0] accepts ALL input channels ([notein N] would silently drop
// every other channel — that filter is correct pd behavior, not a bug).
// [noteout 1] forces the output to channel 0, so hook events always
// report channel 0 regardless of the input channel; pitch/velocity are
// what distinguish the individual notes.
static const char *k_test_patch =
		"#N canvas 0 0 400 200 12;\n"
		"#X obj 10 10 notein 0;\n"
		"#X obj 10 50 noteout 1;\n"
		"#X connect 0 0 1 0;\n"
		"#X connect 0 1 1 1;\n";

struct PatchFile {
	std::string path;
	~PatchFile() {
		std::remove(path.c_str());
	}
};

static PatchFile write_temp_patch() {
	char tmpl[] = "/tmp/libpd_multi_instance.XXXXXX";
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

// Sentinel distinct from any genuine result code: push_and_wait timed
// out waiting for the worker (hung worker). Callers must not treat a
// timeout as a genuine nonzero result (worker_midi_tests convention).
static const int k_command_timeout = std::numeric_limits<int>::min();

/** Push a command that reports its result via promise; return the code
 *  (k_command_timeout if the worker never fulfils it within p_timeout_ms). */
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

/**
 * Convert a hook-level MidiOutMsg into the router's MidiShortMsg form,
 * mirroring the router's output stage (deliver_out_msg): the channel
 * is masked to the low nibble and a velocity-0 NOTE is a NOTE_OFF.
 * RAW_BYTE has no short-message form (raw bytes route as MIDI_BYTE).
 */
static bool out_msg_to_short(const MidiOutMsg &p_msg, MidiShortMsg &r_msg) {
	r_msg = MidiShortMsg{};
	r_msg.channel = p_msg.channel & 0x0F;
	r_msg.d1 = p_msg.d1;
	r_msg.d2 = p_msg.d2;
	switch (p_msg.kind) {
		case MidiOutMsg::NOTE:
			r_msg.kind = p_msg.d2 == 0 ? MidiKind::NOTE_OFF : MidiKind::NOTE_ON;
			return true;
		case MidiOutMsg::CC:
			r_msg.kind = MidiKind::CC;
			return true;
		case MidiOutMsg::PROGRAM_CHANGE:
			r_msg.kind = MidiKind::PROGRAM_CHANGE;
			return true;
		case MidiOutMsg::PITCH_BEND:
			r_msg.kind = MidiKind::PITCH_BEND;
			return true;
		case MidiOutMsg::AFTERTOUCH:
			r_msg.kind = MidiKind::AFTERTOUCH;
			return true;
		case MidiOutMsg::POLY_AFTERTOUCH:
			r_msg.kind = MidiKind::POLY_AFTERTOUCH;
			return true;
		case MidiOutMsg::RAW_BYTE:
			return false;
	}
	return false;
}

/**
 * Mirror of MidiRouter::build_command (src/midi_router.cpp) — that one is
 * private, and this test must not widen the router's API. Keep in sync:
 * i32 = channel passed to libpd unchanged; i64 packing per PdCommand
 * opcode (pitch*256+velocity, controller*256+value, program, low*256+high,
 * pressure*256, pitch*256+pressure).
 */
static bool build_command(const MidiShortMsg &p_msg, PdCommand &r_cmd) {
	r_cmd = PdCommand{};
	r_cmd.i32 = p_msg.channel;
	switch (p_msg.kind) {
		case MidiKind::NOTE_ON:
		case MidiKind::NOTE_OFF:
			r_cmd.opcode = PdCommand::MIDI_NOTE;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::CC:
			r_cmd.opcode = PdCommand::MIDI_CC;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::PROGRAM_CHANGE:
			r_cmd.opcode = PdCommand::MIDI_PROGRAM_CHANGE;
			r_cmd.i64 = p_msg.d1;
			break;
		case MidiKind::PITCH_BEND:
			r_cmd.opcode = PdCommand::MIDI_PITCH_BEND;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::AFTERTOUCH:
			r_cmd.opcode = PdCommand::MIDI_AFTERTOUCH;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256;
			break;
		case MidiKind::POLY_AFTERTOUCH:
			r_cmd.opcode = PdCommand::MIDI_POLY_AFTERTOUCH;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		default:
			return false;
	}
	return true;
}

/**
 * Regression 1: per-instance hook attribution. Each note is pushed to
 * exactly one worker; its hook event must appear in that worker's
 * midi_out and nowhere else.
 */
static void test_attribution(LibpdWorker &a, LibpdWorker &b) {
	// Note through A only -> A's queue, B stays empty.
	{
		PdCommand note;
		note.opcode = PdCommand::MIDI_NOTE;
		note.i32 = 0; // channel 0
		note.i64 = 60 * 256 + 100;
		a.push_command(note);
		std::vector<MidiOutMsg> msgs;
		CHECK(wait_for_msgs(a, 1, 5000, msgs));
		CHECK((int)msgs.size() == 1);
		if (msgs.size() == 1) {
			CHECK(msgs[0].kind == MidiOutMsg::NOTE);
			CHECK(msgs[0].channel == 0);
			CHECK(msgs[0].d1 == 60);
			CHECK(msgs[0].d2 == 100);
		}
		// Settle: a misrouted hook would have reached B immediately.
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::vector<MidiOutMsg> stray;
		CHECK(drain(b, stray) == 0);
		std::printf("attribution: note via A -> A only\n");
	}
	// Reverse direction: note through B only -> B's queue, A stays empty.
	{
		PdCommand note;
		note.opcode = PdCommand::MIDI_NOTE;
		note.i32 = 1; // input channel 1 (distinct from the A case)
		note.i64 = 72 * 256 + 90;
		b.push_command(note);
		std::vector<MidiOutMsg> msgs;
		CHECK(wait_for_msgs(b, 1, 5000, msgs));
		CHECK((int)msgs.size() == 1);
		if (msgs.size() == 1) {
			CHECK(msgs[0].kind == MidiOutMsg::NOTE);
			CHECK(msgs[0].channel == 0); // [noteout 1] normalizes to channel 0
			CHECK(msgs[0].d1 == 72);
			CHECK(msgs[0].d2 == 90);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::vector<MidiOutMsg> stray;
		CHECK(drain(a, stray) == 0);
		std::printf("attribution: note via B -> B only\n");
	}
}

/**
 * Regression 2: routed fan-out at the hook level. Simulates the router
 * loopback A -> port P -> B in-process (no PortMIDI I/O): A's hook
 * event is popped from A's midi_out, converted with the router's real
 * build_command, and delivered to the instances routed from port P.
 * B's own [noteout] hook must fire; A must not receive the note back.
 */
static void test_routed_fanout(LibpdWorker &a, LibpdWorker &b) {
	const int k_port = 7; // synthetic port id; no PortMIDI handle involved
	MidiRoutingTable routes;
	routes.add_route_out(a.instance_id(), k_port); // A's [noteout] -> port P
	routes.add_route_in(k_port, b.instance_id()); // port P -> B's [notein]

	// A emits a note through its own instance.
	PdCommand note;
	note.opcode = PdCommand::MIDI_NOTE;
	note.i32 = 2; // channel 2 (distinct from the attribution cases)
	note.i64 = 64 * 256 + 100;
	a.push_command(note);
	std::vector<MidiOutMsg> msgs;
	CHECK(wait_for_msgs(a, 1, 5000, msgs));
	CHECK((int)msgs.size() == 1);

	// Router output stage, simulated in-process: resolve A's output
	// ports, rebuild each message into a PdCommand (the router's real
	// conversion), and deliver it to every instance routed from the port.
	for (const MidiOutMsg &msg : msgs) {
		CHECK(msg.kind == MidiOutMsg::NOTE);
		const std::vector<int> ports = routes.ports_for_instance(a.instance_id());
		CHECK((int)ports.size() == 1);
		if (ports.size() == 1) {
			CHECK(ports[0] == k_port);
			MidiShortMsg short_msg;
			CHECK(out_msg_to_short(msg, short_msg));
			PdCommand cmd;
			CHECK(build_command(short_msg, cmd));
			const std::vector<int64_t> targets = routes.instances_for_port(ports[0]);
			CHECK((int)targets.size() == 1);
			if (targets.size() == 1) {
				CHECK(targets[0] == b.instance_id());
				// deliver_command -> on_midi_command -> worker queue.
				b.push_command(cmd);
			}
		}
	}

	// B's [notein] received the routed note, so B's [noteout] hook must
	// fire into B's queue (and only B's). B's [noteout 1] normalizes the
	// channel to 0; pitch/velocity must match A's original note.
	std::vector<MidiOutMsg> b_msgs;
	CHECK(wait_for_msgs(b, 1, 5000, b_msgs));
	CHECK((int)b_msgs.size() == 1);
	if (b_msgs.size() == 1) {
		CHECK(b_msgs[0].kind == MidiOutMsg::NOTE);
		CHECK(b_msgs[0].channel == 0);
		CHECK(b_msgs[0].d1 == 64);
		CHECK(b_msgs[0].d2 == 100);
	}
	// Settle: a feedback loop back into A would show up immediately.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::vector<MidiOutMsg> stray;
	CHECK(drain(a, stray) == 0);
	std::printf("routed fan-out: A -> port %d -> B only\n", k_port);
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const PatchFile patch = write_temp_patch();

	LibpdWorker::Config ca;
	ca.instance_id = 1;
	ca.samplerate = 44100;
	ca.n_ins = 0;
	ca.n_out = 0;
	ca.sink = nullptr; // no audio I/O; the tests exercise the message path only
	LibpdWorker a(ca);

	LibpdWorker::Config cb;
	cb.instance_id = 2;
	cb.samplerate = 44100;
	cb.n_ins = 0;
	cb.n_out = 0;
	cb.sink = nullptr;
	LibpdWorker b(cb);

	a.start();
	b.start();

	// INIT both, in order: the first INIT initializes the global pd
	// library; each worker then creates its own pd instance on its own
	// thread. A hung worker must fail, not SKIP (worker_midi_tests M-1).
	PdCommand init_a;
	init_a.opcode = PdCommand::INIT;
	init_a.i32 = 44100;
	init_a.i64 = 0; // n_ins * 1000 + n_out
	const int init_result_a = push_and_wait(a, init_a);
	if (init_result_a == k_command_timeout) {
		std::printf("FAIL: worker A INIT timed out (worker hung)\n%d FAILURES\n", failures);
		a.request_stop();
		a.join();
		b.request_stop();
		b.join();
		return 1;
	}
	if (init_result_a != 0) {
		std::printf("SKIP: no audio for multi-instance midi test (init result %d)\n", init_result_a);
		a.request_stop();
		a.join();
		b.request_stop();
		b.join();
		return 0;
	}

	PdCommand init_b;
	init_b.opcode = PdCommand::INIT;
	init_b.i32 = 44100;
	init_b.i64 = 0;
	const int init_result_b = push_and_wait(b, init_b);
	if (init_result_b == k_command_timeout) {
		std::printf("FAIL: worker B INIT timed out (worker hung)\n%d FAILURES\n", failures);
		a.request_stop();
		a.join();
		b.request_stop();
		b.join();
		return 1;
	}
	if (init_result_b != 0) {
		std::printf("SKIP: no audio for multi-instance midi test (init result %d)\n", init_result_b);
		a.request_stop();
		a.join();
		b.request_stop();
		b.join();
		return 0;
	}

	// LOAD the same patch into both workers.
	PdCommand load_a;
	load_a.opcode = PdCommand::LOAD;
	load_a.path = patch.path;
	const int load_result_a = push_and_wait(a, load_a);
	if (load_result_a == k_command_timeout || load_result_a != 0) {
		std::printf("FAIL: worker A patch load (%s, result %d)\n", patch.path.c_str(), load_result_a);
		failures++;
	}
	PdCommand load_b;
	load_b.opcode = PdCommand::LOAD;
	load_b.path = patch.path;
	const int load_result_b = push_and_wait(b, load_b);
	if (load_result_b == k_command_timeout || load_result_b != 0) {
		std::printf("FAIL: worker B patch load (%s, result %d)\n", patch.path.c_str(), load_result_b);
		failures++;
	}
	if (failures > 0) {
		a.request_stop();
		a.join();
		b.request_stop();
		b.join();
		std::printf("%d FAILURES\n", failures);
		return 1;
	}

	test_attribution(a, b);
	test_routed_fanout(a, b);

	// Teardown on the worker threads (spec §5).
	a.request_stop();
	a.join();
	b.request_stop();
	b.join();

	if (failures == 0) {
		std::printf("ALL MULTI-INSTANCE MIDI TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

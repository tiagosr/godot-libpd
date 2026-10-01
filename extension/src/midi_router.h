#pragma once

// Task 4 — MidiRouter: PortMIDI integration, the MIDI I/O thread, and the
// running-status writer for raw [midiout] byte streams.
//
// Pinned interface: .superpowers/sdd/2026-09-30-godot-libpd-midi/
// task-4-brief.md. Design: docs/superpowers/specs/
// 2026-09-30-godot-libpd-midi-design.md (§1 sysex input-only, §3 threads,
// §5 dual command delivery, §6 output mapping, §7 lifecycle).
//
// PortMIDI 2.0.7 API note (the vendored API differs from the brief's
// illustrative names; full mapping in task-4-report.md):
//   - input: poll Pm_Read(PmEvent*) on the MIDI I/O thread (~1 ms);
//     PmEvent.message carries 1-4 bytes low byte first, and the high bit
//     (0xFF000000) marks a long (sysex) event;
//   - output: full-form short messages as PmMessage words via
//     Pm_WriteShort(stream, 0, Pm_Message(status, d1, d2));
//   - errors: PmError codes, Pm_GetErrorText, and per-stream async host
//     errors via Pm_HasHostError / Pm_GetHostErrorText.
// The MIDI I/O thread is the only code that touches PortMidiStream
// handles, so no Pm_* call ever leaves midi_router.cpp.

#ifdef PORTMIDI_ENABLED
#include <portmidi.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/midi_output_queue.h"
#include "core/midi_routing_table.h"
#include "core/pd_command_queue.h"
#include "core/pd_midi_framer.h"

namespace godot_libpd {

/**
 * One MIDI input event delivered to the main thread via
 * drain_signal_events() (Task 5 emits the GDScript signals from it).
 *
 * One struct, two shapes (pinned): for non-sysex events only `msg` is
 * meaningful; for sysex events only `sysex` is meaningful (the F0..F7
 * body, <= 127 bytes, inclusive of the closing F7).
 */
struct MidiSignalEvent {
	bool is_sysex = false;
	int port_id = 0; // router port id (MidiRouter::open_input)
	MidiShortMsg msg;
	std::vector<uint8_t> sysex;
};

/**
 * Running-status writer for raw [midiout] byte streams (pinned Task 4
 * behavior). Pure: no PortMIDI, no threads, no state beyond the writer —
 * trivially testable (tests/midi_writer_tests.cpp).
 *
 * feed() takes one byte of the raw stream and returns the 0 or 1 wire
 * bytes produced for it:
 *   - a status byte (0x80..0xF7) is emitted only when it differs from
 *     the previously stored status (running status);
 *   - realtime bytes (F8..FF) are always emitted: on the MIDI wire they
 *     are complete one-byte messages, never subject to running status
 *     (a repeated F8 is e.g. a MIDI clock tick — every tick goes out);
 *   - data bytes (0x00..0x7F) are always emitted;
 *   - F0 starts a (dropped) sysex sequence: F0..F7 are swallowed until
 *     the F7 (input-only sysex, spec §1 — there is no sysex output path);
 *   - a non-F0/F7 status byte inside a sysex terminates the truncated
 *     sequence and is then treated as an ordinary status byte.
 *
 * Message boundaries are NOT tracked here; the router (midi_router.cpp)
 * frames complete PmMessage words for Pm_WriteShort on top of this.
 */
struct MidiOutWriter {
	std::vector<uint8_t> feed(uint8_t p_byte) {
		if (in_sysex) {
			if (p_byte == 0xF7) {
				in_sysex = false;
				return {}; // EOX: swallowed, no status change
			} else if (p_byte >= 0x80 && p_byte != 0xF0) {
				// Truncated sysex: fall through; this status is real.
				in_sysex = false;
			} else {
				return {}; // swallow sysex data (and any nested F0)
			}
		}
		if (p_byte >= 0x80) {
			if (p_byte == 0xF0) {
				in_sysex = true; // sysex: dropped whole, resume after F7
				return {};
			}
			if (p_byte >= 0xF8) {
				// Realtime: full-form one-byte message, always emitted.
				// (Does not touch the stored channel status.)
				return {p_byte};
			}
			if (have_status && p_byte == last_status) {
				return {}; // running status: suppressed
			}
			last_status = p_byte;
			have_status = true;
			return {p_byte};
		}
		// Data byte: always emitted. (The router's output framing drops it
		// if it precedes any status byte on that route's stream.)
		return {p_byte};
	}

private:
	bool have_status = false;
	uint8_t last_status = 0;
	bool in_sysex = false;
};

/**
 * The process-wide MIDI router (one instance per process, owned by the
 * Godot extension server — Task 5/6). A dedicated MIDI I/O thread owns
 * all PortMIDI streams; the main thread talks to it through the public
 * methods, all of which are thread-safe. Open/close requests run on the
 * I/O thread via a control queue; the calling thread waits at most 500
 * ms and a timeout is surfaced through on_port_error (spec §3/§7).
 */
class MidiRouter {
public:
	MidiRouter();
	~MidiRouter();

	MidiRouter(const MidiRouter &) = delete;
	MidiRouter &operator=(const MidiRouter &) = delete;

	/**
	 * True when PortMIDI initialized. The device list may legitimately
	 * be empty (A133: the ALSA sequencer exists but exposes no
	 * SUBS-capable ports) — open_virtual_input()/open_virtual_output()
	 * still work, so "available" means "PM backend usable", not
	 * "at least one device exists".
	 */
	bool available() const;

	/**
	 * (pm_device_index, name) for every device with an input (resp.
	 * output) side. PM device indices are stable for the process
	 * lifetime; the UI passes the chosen index to open_input/
	 * open_output.
	 */
	std::vector<std::pair<int, std::string>> list_inputs() const;
	std::vector<std::pair<int, std::string>> list_outputs() const;

	/**
	 * Open PM device p_pm_index as a router input (resp. output) port.
	 * Returns the new router port id (>= 0) or -1 on failure (unknown
	 * device, PM error, open timeout, PortMIDI unavailable). The open
	 * itself runs on the MIDI I/O thread — PM streams are touched there
	 * exclusively; this call waits at most 500 ms and is safe from the
	 * main thread.
	 */
	int open_input(int p_pm_index);
	int open_output(int p_pm_index);

	/**
	 * Create an app-owned PM virtual device (ALSA: an snd_seq virtual
	 * port on the PM client, visible to aconnect; CoreMIDI: a virtual
	 * endpoint) and open it as a router input (resp. output) port.
	 * Returns the new router port id (>= 0) or -1 on failure (create
	 * error, PM error, open timeout, PortMIDI unavailable). The virtual
	 * device is removed (Pm_DeleteVirtualDevice) when the port closes
	 * (close_port) or at shutdown. Same I/O-thread + 500 ms timeout
	 * semantics as open_input/open_output.
	 */
	int open_virtual_input();
	int open_virtual_output();

	/**
	 * Close router port p_port_id (runs on the I/O thread via the
	 * control queue; this call waits at most 500 ms). A close
	 * auto-unroutes the port (in-routes for inputs, out-routes for
	 * outputs). Unknown port ids are a no-op.
	 */
	void close_input(int p_port_id);
	void close_output(int p_port_id);

	/**
	 * Routing (Task 3 tables); main-thread writes, I/O thread snapshots
	 * under lock. Routing a port that is not open is allowed but inert
	 * until the port is opened with that id.
	 */
	void route_input(int p_port_id, int64_t p_instance, bool p_add);
	void route_output(int64_t p_instance, int p_port_id, bool p_add);

	/**
	 * Point the router at p_instance's output queue (worker thread
	 * pushes, I/O thread drains). Idempotent per instance.
	 */
	void register_instance_output(int64_t p_instance, MidiOutputQueue *p_queue);

	/**
	 * Forget an instance: drop its output queue registration and every
	 * route that touches it. Thread-safe; instance teardown order is
	 * Task 5 (forget before the instance and its queue are destroyed).
	 */
	void forget_instance(int64_t p_instance);

	/**
	 * Pull all pending input signal events (clears the internal ring).
	 * Bounded ring: on overflow the OLDEST event is dropped.
	 */
	void drain_signal_events(std::vector<MidiSignalEvent> &r_out);

	/**
	 * Close every open port, Pm_Terminate, and stop the I/O thread.
	 * Idempotent; called from the destructor and from server deinit.
	 */
	void shutdown();

	/**
	 * Per-route command delivery (called on the MIDI I/O thread; Task 5
	 * binds the main-thread consumer). One call per (routed instance,
	 * command). Dual delivery per spec §5: for a decoded short message
	 * the raw MIDI_BYTE commands (one per stream byte, running status
	 * preserved, sysex excluded) precede the high-level command for the
	 * message (the framer emits on_byte per byte before on_short); sysex
	 * arrives as a single MIDI_SYSEX command (F0..F7 inclusive) with no
	 * MIDI_BYTE commands for its bytes. Realtime/system-common bytes
	 * arrive as MIDI_BYTE commands only. Consume (copy) the PdCommand
	 * within the call.
	 *
	 * Re-entrancy contract (enforced by Task 5 binding discipline, not
	 * by this code): fires on the MIDI I/O thread; the bound function
	 * MUST NOT call any MidiRouter method (open_input/open_output/
	 * close_input/close_output/shutdown/route_input/route_output) —
	 * control ops wait for the I/O thread, so calling from the I/O
	 * thread stalls until the 500 ms timeout. Enqueue to the main thread
	 * instead.
	 */
	std::function<void(int64_t p_instance_id, const PdCommand &p_command)> on_midi_command;

	/**
	 * Port-level errors (approved extra member beyond the pinned
	 * interface): (port_id, what), where `what` is Pm_GetErrorText /
	 * host-error text or a short router message, valid during the call
	 * only (do not store the pointer). Fired on the MIDI I/O thread; the
	 * open/close timeout paths fire it from the calling thread. Task 5
	 * binds this to the midi_port_error(port_id, what) signal.
	 * port_id is -1 for router-level errors (failed open, timeout).
	 *
	 * Re-entrancy contract (enforced by Task 5 binding discipline, not
	 * by this code): the bound function MUST NOT call any MidiRouter
	 * method (open_input/open_output/close_input/close_output/shutdown/
	 * route_input/route_output); enqueue to the main thread instead.
	 */
	std::function<void(int p_port_id, const char *p_what)> on_port_error;

private:
	// ------------------------------------------------------------------
	// Control queue (API thread -> I/O thread): open/close/shutdown ops
	// with per-op completion promises.
	// ------------------------------------------------------------------

	enum class ControlOpType {
		OPEN_INPUT,
		OPEN_OUTPUT,
		// I/O thread creates the PM virtual device first, then opens the
		// returned device id (the id is only known after creation, so
		// these ops cannot reuse OPEN_*'s pm_index parameter).
		OPEN_VIRTUAL_INPUT,
		OPEN_VIRTUAL_OUTPUT,
		CLOSE_INPUT,
		CLOSE_OUTPUT,
		SHUTDOWN,
	};

	struct ControlOp {
		ControlOpType op = ControlOpType::SHUTDOWN;
		int pm_index = -1; // OPEN_*: PM device index
		int port_id = -1; // CLOSE_*: router port id
		// Set by the API side when its 500 ms wait expired before the op
		// finished; a late open success then self-closes the port.
		std::shared_ptr<std::atomic<bool>> abandoned;
		// Fulfilled by the I/O thread when the op completes (value: new
		// port id for open, 0 for close, -1 for failure).
		std::shared_ptr<std::promise<int>> done;
	};

	struct ControlHandle {
		std::shared_ptr<std::atomic<bool>> abandoned;
		std::future<int> result;
	};

	ControlHandle enqueue_control(ControlOpType p_op, int p_pm_index, int p_port_id);

	// ------------------------------------------------------------------
	// Per-port state. Port records live in ports_ for the router's
	// lifetime (port ids are never reused); all mutation happens on the
	// I/O thread, so the records need no lock after creation.
	// ------------------------------------------------------------------

	/**
	 * Bounded raw-byte ring for one input port (spec §3 "bounded
	 * everywhere"). The Pm_Read stage pushes; the framer stage drains
	 * fully within the same I/O loop iteration (same thread). On
	 * overflow the OLDEST byte is dropped (cannot happen in practice:
	 * one Pm_Read batch is <= 128 bytes against a 4096 cap).
	 */
	struct ByteRing {
		static constexpr int CAP = 4096;

		void push(uint8_t p_byte) {
			int next = (head + count) % CAP;
			data[next] = p_byte;
			if (count < CAP) {
				++count;
			} else {
				head = next; // full: overwrite the oldest byte
			}
		}

		/** Feed every buffered byte (in order) to p_fn, then clear. */
		template <typename Fn>
		void drain(Fn p_fn) {
			for (int i = 0; i < count; ++i) {
				p_fn(data[(head + i) % CAP]);
			}
			head = 0;
			count = 0;
		}

		void reset() {
			head = 0;
			count = 0;
		}

		uint8_t data[CAP] = {};
		int head = 0;
		int count = 0;
	};

	/**
	 * Per-input-port framer + sink (MidiFramingSink implementation that
	 * delegates to the router). The framer holds the sink by reference
	 * for its lifetime, so both live in this small object.
	 */
	struct PortInput : MidiFramingSink {
		PortInput(MidiRouter *p_router, int p_port_id) :
			router(p_router),
			port_id(p_port_id),
			framer(*this) {}

		void on_short(const MidiShortMsg &p_msg) override {
			router->on_input_short(port_id, p_msg);
		}
		void on_byte(uint8_t p_byte) override {
			router->on_input_byte(port_id, p_byte);
		}
		void on_sysex(const uint8_t *p_data, int p_len) override {
			router->on_input_sysex(port_id, p_data, p_len);
		}
		void on_sysex_truncated() override {
			router->on_input_sysex_truncated(port_id);
		}

		MidiRouter *router;
		int port_id;
		MidiFramer framer;
	};

	struct Port {
		bool in_use = false;
		bool is_input = false;
		int pm_index = -1;
		// This port created its PM virtual device (open_virtual_*): the
		// device must be Pm_DeleteVirtualDevice'd after the stream
		// closes (close_port / io_loop exit). Pm_Close alone does not
		// remove it (ALSA keeps virtual ports open on purpose — the
		// port IS the device).
		bool pm_device_owned = false;
#ifdef PORTMIDI_ENABLED
		PortMidiStream *pm_stream = nullptr;
#endif
		ByteRing ring; // input only: read stage -> framer stage
		PortInput *input = nullptr; // input only: owns framer + sink
	};

	/**
	 * Raw-byte output framing state per (instance, port) route. I/O
	 * thread only (raw_writers_ is touched exclusively there). Mirrors
	 * the MidiOutWriter sysex state independently so message completion
	 * can be tracked for Pm_WriteShort (which requires a full-form
	 * status byte; running-status input is re-expanded here).
	 */
	struct RawRouteState {
		MidiOutWriter writer;
		uint8_t status = 0; // last status byte on this route's stream
		bool have_status = false;
		bool in_sysex = false;
		uint8_t data[2] = {0, 0};
		int data_have = 0;
		int data_need = 0; // data bytes expected after the status byte
	};

	// ------------------------------------------------------------------
	// Members
	// ------------------------------------------------------------------

	// Guards: ports_, instances_, out_port_instances_, signal_events_,
	// next_port_id_. No Pm_* call is ever made while a router lock is
	// held (PM drivers never re-enter router code; keeping the critical
	// sections tight also avoids API-thread stalls). Port records and
	// raw_writers_ are mutated exclusively on the I/O thread.
	std::mutex mutex_;
	std::unordered_map<int, std::unique_ptr<Port>> ports_;
	struct InstanceOut {
		MidiOutputQueue *queue = nullptr; // registered on the API thread;
		// popped by the I/O thread under mutex_ (queue lifetime is
		// bounded by Task 5 teardown order: forget before destroy)
	};
	std::unordered_map<int64_t, InstanceOut> instances_;
	// port -> instances routed to it (mirror of out_routes_ so a close
	// can auto-unroute the port; guarded by mutex_)
	std::unordered_map<int, std::unordered_set<int64_t>> out_port_instances_;
	MidiRoutingTable in_routes_;
	MidiRoutingTable out_routes_;
	std::vector<MidiSignalEvent> signal_events_; // bounded, drop-oldest
	int next_port_id_ = 0; // port ids are never reused (monotonic)

	// Raw-byte output framing per (instance, port): I/O thread only.
	std::unordered_map<int64_t, std::unordered_map<int, RawRouteState>> raw_writers_;

	// Control-queue side (guarded by control_mutex_).
	std::mutex control_mutex_;
	std::condition_variable control_cv_; // thread-exit notification
	bool thread_running_ = false;
	bool thread_done_ = false;
	std::deque<ControlOp> control_queue_;

#ifdef PORTMIDI_ENABLED
	bool pm_initialized_ = false; // Pm_Initialize result (constructor)
	// PortMIDI is NOT thread-safe (portmidi.h: "you cannot allow threads
	// to call PortMidi functions concurrently"). Held around EVERY Pm_*
	// call on both the API thread and the I/O thread; the ~1 ms poll
	// makes contention negligible. notify_port_error (user callback) is
	// never invoked while this lock is held.
	mutable std::mutex pm_api_mutex_;
#endif
	std::thread io_thread_;

#ifdef PORTMIDI_ENABLED
	// Runs p_fn holding pm_api_mutex_ (see above). Every Pm_* call on
	// both threads goes through this; user callbacks are invoked only
	// after the lock is released.
	template <typename Fn>
	auto with_pm_mutex(Fn &&p_fn) const -> decltype(p_fn()) {
		std::lock_guard<std::mutex> lock(pm_api_mutex_);
		return p_fn();
	}
#endif

	// ------------------------------------------------------------------
	// I/O thread
	// ------------------------------------------------------------------

	void io_loop();
	bool process_control_ops(); // true -> exit the loop (shutdown handled)
	int open_port(bool p_is_input, int p_pm_index); // I/O thread
	int open_virtual_port(bool p_is_input); // I/O thread
	void close_port(int p_port_id); // I/O thread
	void handle_read_error(int p_port_id, int p_err); // I/O thread
	void output_stage(); // I/O thread
#ifdef PORTMIDI_ENABLED
	// Return value: false after a failed Pm_WriteShort — the port is
	// already closed and unrouted (write_short ran close_port); the
	// caller must stop writing to that stream and drop the rest of the
	// batch (writing to a closed stream is UB).
	bool deliver_out_msg(PortMidiStream *p_stream, int p_port_id,
			std::unordered_map<int, RawRouteState> &p_writers,
			const MidiOutMsg &p_msg); // I/O thread
	bool raw_byte_to_stream(PortMidiStream *p_stream, int p_port_id,
			std::unordered_map<int, RawRouteState> &p_writers,
			uint8_t p_byte); // I/O thread
	bool write_short(PortMidiStream *p_stream, int p_port_id, PmMessage p_msg); // I/O thread
#endif

	Port *port_ptr(int p_port_id);

	// ------------------------------------------------------------------
	// Input fan-out (called from PortInput sink callbacks, I/O thread)
	// ------------------------------------------------------------------

	void on_input_short(int p_port_id, const MidiShortMsg &p_msg);
	void on_input_byte(int p_port_id, uint8_t p_byte);
	void on_input_sysex(int p_port_id, const uint8_t *p_data, int p_len);
	void on_input_sysex_truncated(int p_port_id);
	void push_signal_event(const MidiSignalEvent &p_event);
	void deliver_command(int64_t p_instance, const PdCommand &p_cmd);
	static bool build_command(const MidiShortMsg &p_msg, PdCommand &r_cmd);

	// ------------------------------------------------------------------
	// Error notification (I/O thread; text valid during the call only)
	// ------------------------------------------------------------------

	void notify_port_error(int p_port_id, const char *p_what);
};

} // namespace godot_libpd

#pragma once

// Task 4 — MidiRouter: the MIDI I/O thread, and the running-status
// writer for raw [midiout] byte streams.
//
// Pinned interface: .superpowers/sdd/2026-09-30-godot-libpd-midi/
// task-4-brief.md. Design: docs/superpowers/specs/
// 2026-09-30-godot-libpd-midi-design.md (§1 sysex input-only, §3 threads,
// §5 dual command delivery, §6 output mapping, §7 lifecycle).
//
// v2 M2 Task 1: all platform MIDI calls live behind the MidiBackend
// interface (midi_backend.h) — PortMidiBackend on macOS/Linux (the
// Pm_* logic extracted from this file), the Android RtMidi backend in
// a later task. The router is backend-agnostic: it owns routing state,
// the rings, the I/O thread, and the API, and talks to the backend
// through the interface only. The platform backend is built by
// create_midi_backend() (midi_backend_portmidi.cpp); tests inject a
// fake backend through the constructor.
//
// Word layout (inherited from the vendored PortMIDI 2.0.7, now the
// interface contract in midi_backend.h): PmEvent.message is a plain
// "up to 4 bytes, low byte first" word with NO long-event flag (the
// vendored backend never sets 0xFF000000): short events start at byte
// 0; sysex arrives as an F0-first run of words terminated by an F7
// that zero-pads the rest of its word — the per-port MidiReadStage
// re-assembles the words into raw stream bytes before the framer
// frames them. Output: full-form short messages as {status, d1, d2}
// byte triples handed to MidiBackend::write (PortMidi maps them to
// Pm_Message words).
// The MIDI I/O thread is the only code that touches open backend
// handles, so no backend port operation ever leaves the I/O thread.

#ifdef PORTMIDI_ENABLED
// Kept for midi_open_bounds_tests.cpp, which includes only this
// header and calls Pm_CountDevices() directly. The router itself no
// longer touches Pm_* (that is PortMidiBackend's job).
#include <portmidi.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <map>
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
#include "midi_backend.h"

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
 * Per-port sysex read-stage state machine (pinned Task 4 interface,
 * reworked for the vendored PortMIDI word layout — see file header).
 * Pure: no platform MIDI, no threads, fixed state, allocation-free —
 * trivially testable (tests/midi_read_stage_tests.cpp).
 *
 * The backend delivers words as plain little-endian "up to 4 bytes,
 * low byte first" — NO long-event flag: short events start at byte 0
 * (status + 0-2 data bytes, zero padding after); sysex arrives as a
 * run of words where the first word has F0 in byte 0, continuation
 * words carry up to 4 data bytes, and the final word carries F7 at
 * byte k with zero padding after it (the backend enqueues the word at
 * F7 and resets, so the padding is always zero in practice).
 *
 * feed() converts one word into raw stream bytes and pushes them via
 * p_push in wire order; the router wires p_push to the per-port
 * ByteRing, and the framer re-frames the stream (running status,
 * short-message decode, sysex buffering).
 *
 * Sysex signal contract (pinned, verified against pd_midi_framer.cpp
 * and LibpdServer): the framer's on_sysex body is F0-EXCLUSIVE and
 * F7-INCLUSIVE (capped at 127 bytes); the server prepends the F0, so
 * the emitted `midi_sysex` signal data is exactly one full F0..F7
 * message.
 */
struct MidiReadStage {
	// True while an F0..F7 message is open (F0 seen, F7 not yet).
	bool in_sysex = false;

	/**
	 * Feed one backend word (low byte first); the resulting stream bytes
	 * are pushed via p_push (e.g. ByteRing::push) in wire order.
	 */
	template <typename Fn>
	void feed(uint32_t p_message, Fn p_push) {
		const uint8_t b[4] = {
				static_cast<uint8_t>(p_message & 0xFF),
				static_cast<uint8_t>((p_message >> 8) & 0xFF),
				static_cast<uint8_t>((p_message >> 16) & 0xFF),
				static_cast<uint8_t>((p_message >> 24) & 0xFF),
		};
		if (in_sysex || b[0] == 0xF0) {
			in_sysex = true;
			feed_sysex_word(b, p_push);
			return;
		}
		// Short event: status + 0-2 data bytes, low byte first.
		const int n = pm_short_bytes(b[0]);
		for (int i = 0; i < n; ++i) {
			p_push(b[i]);
		}
	}

	/** Drop pending sysex state (port close / buffer-overflow reset). */
	void reset() {
		in_sysex = false;
	}

	/**
	 * Total bytes a short (non-sysex) event carries: vendored
	 * pm_midi_length() semantics (pm_common/portmidi.c). F4..F7 are
	 * single-byte system-common messages (tune request, end of cable,
	 * RT reset, ...) — NOT 2-byte messages.
	 */
	static int pm_short_bytes(uint8_t p_status) {
		if (p_status < 0x80) {
			return 1; // not expected for short events; safe default
		}
		const uint8_t type = p_status & 0xF0;
		if (type == 0xC0 || type == 0xD0) {
			return 2; // program change, channel aftertouch
		}
		if (type == 0xF0) {
			if (p_status == 0xF2) {
				return 3; // song position pointer
			}
			if (p_status >= 0xF4) {
				return 1; // F4..F7: single-byte system common
			}
			return 2; // F1 (MTC quarter frame), F3 (song select)
		}
		if (type == 0xF8) {
			return 1; // realtime F8..FF
		}
		return 3; // 0x80..0xBF (channel), 0xE0 (pitch bend)
	}

	/**
	 * Data bytes expected after a status byte in a raw [midiout] stream
	 * (total message bytes - 1). Must EXACTLY mirror the vendored
	 * pm_midi_length() table for round-trip consistency with PM's own
	 * input parser — note upstream counts F1 as a 2-byte message and
	 * F2 as 3 bytes (not the strict MIDI-spec counts). F0 never
	 * reaches this: the caller enters the sysex drop path first.
	 */
	static int raw_data_need(uint8_t p_status) {
		if (p_status < 0x80) {
			return 0; // not a status byte
		}
		return pm_short_bytes(p_status) - 1;
	}

private:
	// Sysex word (start or continuation): push stream bytes up to and
	// including the F7. If the F7 lands at byte k < 3, bytes k+1..3 are
	// zero padding in the vendored backend (enqueued at F7, then reset)
	// and are never emitted; if a non-zero byte follows the F7, it
	// starts the NEXT event: status (>= 0x80) -> a short event, of which
	// only the bytes present in this word are emitted; data (< 0x80) ->
	// one running-status continuation byte.
	template <typename Fn>
	void feed_sysex_word(const uint8_t *p_b, Fn p_push) {
		for (int k = 0; k < 4; ++k) {
			p_push(p_b[k]);
			if (p_b[k] != 0xF7) {
				continue;
			}
			in_sysex = false;
			const int first = k + 1;
			if (first >= 4 || p_b[first] == 0) {
				return; // zero padding after the F7
			}
			if (p_b[first] >= 0x80) {
				const int need = pm_short_bytes(p_b[first]);
				int have = 4 - first;
				if (need < have) {
					have = need;
				}
				for (int i = first; i < first + have; ++i) {
					p_push(p_b[i]);
				}
			} else {
				p_push(p_b[first]);
			}
			return;
		}
	}
};

/**
 * Running-status writer for raw [midiout] byte streams (pinned Task 4
 * behavior). Pure: no platform MIDI, no threads, no state beyond the
 * writer — trivially testable (tests/midi_writer_tests.cpp).
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
 *     the F7 (input-only sysex, spec §1 — there is no sysex output path
 *     on the raw-byte route; full sysex goes out via MidiOutMsg::SYSEX);
 *   - a non-F0/F7 status byte inside a sysex terminates the truncated
 *     sequence and is then treated as an ordinary status byte.
 *
 * Message boundaries are NOT tracked here; the router (midi_router.cpp)
 * frames complete messages for MidiBackend::write on top of this.
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
 * all backend port handles; the main thread talks to it through the
 * public methods, all of which are thread-safe. Open/close requests
 * run on the I/O thread via a control queue; the calling thread waits
 * at most 500 ms and a timeout is surfaced through on_port_error
 * (spec §3/§7).
 */
class MidiRouter {
public:
	/**
	 * Default construction: the platform backend from
	 * create_midi_backend() (PortMidi on macOS/Linux, nullptr on the
	 * stub/Android hosts — see midi_backend_portmidi.cpp). If the
	 * backend initializes, the I/O thread starts; otherwise the router
	 * degrades to the inert stub (available() false, opens fail).
	 */
	MidiRouter();
	/**
	 * Construction with an injected backend (test seam — e.g. the
	 * in-process fake backend in tests/midi_backend_fake.cpp). Same
	 * init + I/O-thread semantics as the default constructor.
	 */
	explicit MidiRouter(std::unique_ptr<MidiBackend> p_backend);
	~MidiRouter();

	MidiRouter(const MidiRouter &) = delete;
	MidiRouter &operator=(const MidiRouter &) = delete;

	/**
	 * True when the MIDI backend initialized. The device list may
	 * legitimately be empty (A133: the ALSA sequencer exists but
	 * exposes no SUBS-capable ports) — open_virtual_input()/
	 * open_virtual_output() still work, so "available" means "backend
	 * usable", not "at least one device exists".
	 */
	bool available() const;

	/**
	 * Set the hotplug re-enumeration cadence in seconds (M4; default 0.5).
	 * Thread-safe: the I/O loop reads the atomic each tick. A smaller
	 * value detects plug/unplug sooner at the cost of a more frequent
	 * (cheap) list_ports() call.
	 */
	void set_poll_interval(double p_seconds);

	/**
	 * (backend device index, name) for every device with an input
	 * (resp. output) side. Backend device indices are stable for the
	 * process lifetime; the UI passes the chosen index to open_input/
	 * open_output.
	 */
	std::vector<std::pair<int, std::string>> list_inputs() const;
	std::vector<std::pair<int, std::string>> list_outputs() const;

	/**
	 * Open backend device p_pm_index as a router input (resp. output)
	 * port. Returns the new router port id (>= 0) or -1 on failure
	 * (unknown device, backend error, open timeout, backend
	 * unavailable). The open itself runs on the MIDI I/O thread —
	 * backend streams are touched there exclusively; this call waits at
	 * most 500 ms and is safe from the main thread.
	 */
	int open_input(int p_pm_index);
	int open_output(int p_pm_index);

	/**
	 * Create an app-owned backend virtual device (ALSA: an snd_seq
	 * virtual port on the backend client, visible to aconnect;
	 * CoreMIDI: a virtual endpoint) and open it as a router input
	 * (resp. output) port. Returns the new router port id (>= 0) or -1
	 * on failure (create error, backend error, open timeout, backend
	 * unavailable). The virtual device is removed when the port closes
	 * (close_port) or at shutdown. Same I/O-thread + 500 ms timeout
	 * semantics as open_input/open_output.
	 */
	int open_virtual_input();
	int open_virtual_output();

	/**
	 * Create the backend's in-process virtual loopback (design spec
	 * §3): one input port and one output port wired together, reported
	 * by list_inputs()/list_outputs() under the given name (+" in" /
	 * +" out"). Opens of the returned loopback then go through the
	 * ordinary open_input()/open_output() with the listed device
	 * indices. Returns 0 on success, -1 on failure (backend error /
	 * timeout; the PortMidi backend always fails — use IAC/aconnect
	 * there). Same I/O-thread + 500 ms timeout semantics.
	 */
	int create_virtual_loopback(const std::string &p_name);

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
	 * Close every open port, shut down the backend, and stop the I/O
	 * thread. Idempotent; called from the destructor and from server
	 * deinit.
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
	 * interface): (port_id, what), where `what` is backend error text
	 * (PortMidi: Pm_GetErrorText / host-error text, composed by the
	 * backend including the " (pm device N)" open suffix) or a short
	 * router message, valid during the call only (do not store the
	 * pointer). Fired on the MIDI I/O thread; the open/close timeout
	 * paths fire it from the calling thread. Task 5 binds this to the
	 * midi_port_error(port_id, what) signal.
	 * port_id is -1 for router-level errors (failed open, timeout).
	 *
	 * Re-entrancy contract (enforced by Task 5 binding discipline, not
	 * by this code): the bound function MUST NOT call any MidiRouter
	 * method (open_input/open_output/close_input/close_output/shutdown/
	 * route_input/route_output); enqueue to the main thread instead.
	 */
	std::function<void(int p_port_id, const char *p_what)> on_port_error;

	/**
	 * Hotplug port-change notification (M4). Fires on the MIDI I/O thread
	 * when the periodic (or forced) re-enumeration detects a device
	 * appearing (p_added == true) or disappearing (p_added == false).
	 * p_kind is "input" or "output"; p_index is the current index (add)
	 * or the last remembered index (remove); p_name is the port name
	 * (valid during the call only — copy it).
	 *
	 * Re-entrancy contract (same as on_port_error): the bound function
	 * MUST NOT call any MidiRouter method; enqueue to the main thread.
	 */
	std::function<void(bool p_added, const char *p_kind, int p_index, const char *p_name)> on_port_changed;

private:
	// ------------------------------------------------------------------
	// Control queue (API thread -> I/O thread): open/close/shutdown ops
	// with per-op completion promises.
	// ------------------------------------------------------------------

	enum class ControlOpType {
		OPEN_INPUT,
		OPEN_OUTPUT,
		// I/O thread creates the backend virtual device first, then
		// opens the returned device id (the id is only known after
		// creation, so these ops cannot reuse OPEN_*'s pm_index
		// parameter).
		OPEN_VIRTUAL_INPUT,
		OPEN_VIRTUAL_OUTPUT,
		// Backend in-process loopback (name travels with the op).
		CREATE_LOOPBACK,
		CLOSE_INPUT,
		CLOSE_OUTPUT,
		SHUTDOWN,
	};

	struct ControlOp {
		ControlOpType op = ControlOpType::SHUTDOWN;
		int pm_index = -1; // OPEN_*: backend device index
		int port_id = -1; // CLOSE_*: router port id
		std::string name; // CREATE_LOOPBACK: loopback port name
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

	ControlHandle enqueue_control(ControlOpType p_op, int p_pm_index, int p_port_id,
				const std::string &p_name = "");

	// ------------------------------------------------------------------
	// Per-port state. Port records live in ports_ for the router's
	// lifetime (port ids are never reused); all mutation happens on the
	// I/O thread, so the records need no lock after creation.
	// ------------------------------------------------------------------

	/**
	 * Bounded raw-byte ring for one input port (spec §3 "bounded
	 * everywhere"). The poll stage pushes; the framer stage drains
	 * fully within the same I/O loop iteration (same thread). On
	 * overflow the OLDEST byte is dropped (cannot happen in practice:
	 * one poll batch is <= 128 bytes against a 4096 cap).
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

	// Hotplug diff key: (direction, name). Indices are volatile across
	// device changes, so the diff keys by name, not index.
	struct PortKey {
		bool is_input = false;
		std::string name;
		bool operator<(const PortKey &o) const {
			if (is_input != o.is_input) {
				return is_input < o.is_input;
			}
			return name < o.name;
		}
	};

	struct Port {
		bool in_use = false;
		bool is_input = false;
		// Open backend stream (MidiBackend::open_*). The backend owns
		// the stream and any virtual device it created for this port;
		// close_port/shutdown release it through MidiBackend::close /
		// MidiBackend::shutdown (I/O thread only).
		MidiBackend::PortHandle backend_handle = MidiBackend::NO_HANDLE;
		// Hotplug (M4): true only for ports backed by an OS/device endpoint
		// that can hot-plug (set in open_port from non_real_indices_). Only
		// real_device ports are auto-closed by the diff; virtual/loopback
		// are app-owned and never closed by it.
		bool real_device = false;
		// Backend port name captured at open time; used to match a removed
		// (direction, name) key to this port (index is NOT used — volatile).
		std::string device_name;
		ByteRing ring; // input only: read stage -> framer stage
		MidiReadStage read_stage; // input only: backend word -> stream bytes (sysex state)
		PortInput *input = nullptr; // input only: owns framer + sink
	};

	/**
	 * Raw-byte output framing state per (instance, port) route. I/O
	 * thread only (raw_writers_ is touched exclusively there). Mirrors
	 * the MidiOutWriter sysex state independently so message completion
	 * can be tracked for MidiBackend::write (which requires full-form
	 * status bytes; running-status input is re-expanded here).
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

	// The platform MIDI backend (PortMidi on macOS/Linux; nullptr in
	// stub builds and on Android until the RtMidi backend lands). The
	// router holds it for its lifetime; the I/O thread owns all port
	// operations on it, the API thread calls only the const
	// list_ports()/available() methods. backend_->last_error() is
	// read on the I/O thread only (backend contract).
	std::unique_ptr<MidiBackend> backend_;

	// Guards: ports_, instances_, out_port_instances_, signal_events_,
	// next_port_id_. No backend call is made while a router lock is
	// held (backend calls take their own locks, which may block on the
	// I/O thread; keeping the critical sections tight also avoids
	// API-thread stalls). Port records and raw_writers_ are mutated
	// exclusively on the I/O thread.
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

	// Hotplug (M4) diff state — mutated ONLY on the I/O thread (except the
	// atomic poll_interval_, which the API thread writes).
	std::chrono::steady_clock::time_point last_reeum_;
	// NOTE: last_reeum_ is set to the construction time in the constructor
	// (before the I/O thread starts) so the FIRST re-enumeration tick is
	// deferred by one poll interval — this gives the consumer time to bind
	// on_port_changed before the startup (annotated) baseline fires. The
	// default member init (time_point::min()) would otherwise fire the
	// first tick immediately into an unbound callback.
	std::atomic<double> poll_interval_ {0.5};
	std::set<PortKey> seen_; // last-enumerated (direction, name) set
	std::map<PortKey, int> last_index_; // remembered index per key (volatile)
	bool pending_empty_ = false; // transient-empty debounce (2-consecutive-empty)
	std::set<int> non_real_indices_; // virtual/loopback indices (never auto-closed)

	// Raw-byte output framing per (instance, port): I/O thread only.
	std::unordered_map<int64_t, std::unordered_map<int, RawRouteState>> raw_writers_;

	// Control-queue side (guarded by control_mutex_).
	std::mutex control_mutex_;
	std::condition_variable control_cv_; // thread-exit notification
	bool thread_running_ = false;
	bool thread_done_ = false;
	std::deque<ControlOp> control_queue_;
	std::thread io_thread_;

	// ------------------------------------------------------------------
	// I/O thread
	// ------------------------------------------------------------------

	void io_loop();
	bool process_control_ops(); // true -> exit the loop (shutdown handled)
	int open_port(bool p_is_input, int p_device_index); // I/O thread
	int open_virtual_port(bool p_is_input); // I/O thread
	void close_port(int p_port_id); // I/O thread
	void handle_poll_error(int p_port_id, MidiBackend::PollResult p_result); // I/O thread
	// Hotplug (M4): re-enumerate + diff (I/O thread; called by io_loop and
	// by the REFRESH control op). Returns the number of changed ports
	// (added + removed).
	int reenum_and_diff(); // I/O thread
	// Close every open real_device port matching (direction, name), firing
	// notify_port_error(pid, "device removed") for each (I/O thread).
	void close_real_device_ports(bool p_is_input, const std::string &p_name); // I/O thread
	void output_stage(); // I/O thread
	// Return value: false after a failed backend write — for stream
	// failures the port is already closed and unrouted (write_short /
	// write_sysex_msg ran close_port); the caller must stop writing to
	// that port and drop the rest of the batch (writing to a closed
	// stream would be UB).
	bool deliver_out_msg(MidiBackend::PortHandle p_handle, int p_port_id,
			std::unordered_map<int, RawRouteState> &p_writers,
			const MidiOutMsg &p_msg); // I/O thread
	bool raw_byte_to_stream(MidiBackend::PortHandle p_handle, int p_port_id,
			std::unordered_map<int, RawRouteState> &p_writers,
			uint8_t p_byte); // I/O thread
	bool write_short(MidiBackend::PortHandle p_handle, int p_port_id,
			uint8_t p_status, uint8_t p_d1, uint8_t p_d2); // I/O thread
	bool write_sysex_msg(MidiBackend::PortHandle p_handle, int p_port_id,
			const std::vector<uint8_t> &p_sysex); // I/O thread

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

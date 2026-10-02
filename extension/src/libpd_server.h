#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pd_event_ring.h"
#include "midi_router.h"

namespace godot {

class LibpdInstance;

/**
 * Project-level singleton (used via the `Libpd` autoload wrapper in the test
 * project; exactly one instance per project).
 *
 * Owns the cross-thread event ring: worker threads push PdEvents, the server
 * drains the ring in _process() (main thread only) and emits Godot signals.
 * No worker thread ever calls into Godot — this is the only thread-crossing
 * mechanism, which keeps the design deadlock-free (spec §4/§5).
 *
 * Also owns the process-wide MidiRouter (MIDI I/O thread, PortMIDI ports,
 * input routing to registered LibpdInstances). The I/O thread only enqueues
 * (pull: drain_signal_events() + a pending port-error list); the typed
 * midi_* signals are emitted from _process() like the instance_* signals.
 */
class LibpdServer : public Node {
	GDCLASS(LibpdServer, Node)

public:
	LibpdServer();
	virtual ~LibpdServer();

	/// Most recently created server (the project uses exactly one).
	static LibpdServer *get_singleton();

	/// True if the instance id was registered by a LibpdInstance node.
	bool instance_registered(int64_t p_instance_id) const;
	/// Number of registered instances.
	int instance_count() const;

	/// Test hook: push a PRINT event directly into the ring (used by the
	/// headless integration tests; real events arrive via the libpd hooks).
	void debug_push_print(int64_t p_instance_id, const String &p_text);

	/// Push an arbitrary event (called from worker threads via the libpd hooks).
	void push_event(const godot_libpd::PdEvent &p_event);

	/// Register an instance: it becomes the router's MIDI command target
	/// (I/O thread -> push_midi_command) and its worker output queue is
	/// registered with the router. Called from LibpdInstance::_enter_tree.
	void register_instance(LibpdInstance *p_instance);
	/// Unregister an instance: drop it from the command-target map and call
	/// router.forget_instance() (unroute + drop the output queue). Called
	/// from LibpdInstance::_exit_tree.
	void unregister_instance(LibpdInstance *p_instance);

	// ------------------------------------------------------------------
	// MIDI I/O API (spec §3; main thread only).
	// Stub builds (no PortMIDI / BUILD_PORTMIDI=OFF): midi_available()
	// is false and every other method push_error()s and returns
	// -1/no-op; the signals stay declared but never fire.
	// ------------------------------------------------------------------

	/// True when PortMIDI is initialized (the device list may be empty;
	/// virtual ports can still be created — see midi_open_virtual_*).
	bool midi_available();
	/// PM device indices + names of devices with an input side.
	/// NOTE (M4 hotplug): indices are stable only while the device set is
	/// unchanged — re-query after any midi_port_added / midi_port_removed.
	Array midi_list_inputs();
	/// PM device indices + names of devices with an output side. (Same
	/// index-stability note as midi_list_inputs.)
	Array midi_list_outputs();
	/// Open PM device p_pm_index as an input port.
	/// Returns the new port id (>= 0) or -1 on failure.
	int midi_open_input(int p_pm_index);
	/// Open PM device p_pm_index as an output port.
	/// Returns the new port id (>= 0) or -1 on failure.
	int midi_open_output(int p_pm_index);
	/// Create + open an app-owned virtual input port ("libpd test app
	/// in 0"; on ALSA an snd_seq virtual port visible in aconnect -l).
	/// Returns the new port id (>= 0) or -1 on failure. The virtual
	/// device is removed when the port closes (or at server deinit).
	int midi_open_virtual_input();
	/// Create + open an app-owned virtual output port ("libpd test app
	/// out 0"; on ALSA an snd_seq virtual port visible in aconnect -l).
	/// Returns the new port id (>= 0) or -1 on failure.
	int midi_open_virtual_output();
	/// Create the backend's in-process virtual loopback (M2 spec §3;
	/// the Android RtMidi backend — no virtual MIDI device API there).
	/// The pair appears in midi_list_inputs()/midi_list_outputs() as
	/// "<p_name> in" / "<p_name> out" (device indices 200/201); open
	/// them with the ordinary midi_open_input()/midi_open_output().
	/// PortMidi hosts: unavailable (use IAC / aconnect). Returns 0 on
	/// success, -1 on failure.
	int midi_create_loopback(const String &p_name);
	/// Close an open input port (errors surface via the midi_port_error signal).
	void midi_close_input(int p_port_id);
	/// Close an open output port.
	void midi_close_output(int p_port_id);
	/// Route input port p_port_id to p_instance (p_add = false unroutes).
	/// Routing is allowed even while the port is closed; it becomes
	/// effective when the port opens.
	void midi_route_input(int p_port_id, LibpdInstance *p_instance, bool p_add);
	/// Route p_instance's MIDI output to output port p_port_id.
	void midi_route_output(LibpdInstance *p_instance, int p_port_id, bool p_add);

	// Hotplug (M4): live device add/remove. midi_port_added / midi_port_removed
	// fire (from _process) when the periodic re-enumeration detects a change.
	/// Force an immediate re-enumeration + diff; returns the number of changed
	/// ports (added + removed), or -1 if MIDI is unavailable / timed out.
	int midi_refresh_ports();
	/// Set the hotplug re-enumeration cadence in seconds (default 0.5).
	void midi_set_poll_interval(double p_seconds);
	/// Get the hotplug re-enumeration cadence in seconds.
	double midi_get_poll_interval() const;

	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	/// One midi_port_error signal waiting for emission on the main thread.
	struct PortErrorEvent {
		int port_id = -1;
		String what;
	};

	/// One midi_port_added / midi_port_removed signal waiting for emission on
	/// the main thread (hotplug, M4).
	struct PortEvent {
		bool added = false;
		String kind; // "input" or "output"
		int index = -1;
		String name;
	};

	void _drain_ring();
	void _drain_midi_events();

	godot_libpd::PdEventRing ring;
	// Process-wide MIDI router (Task 4). Its constructor starts the MIDI I/O
	// thread (PortMIDI-enabled builds); its destructor joins it. Callbacks
	// are wired in the server constructor; the main-thread side is
	// _drain_midi_events().
	godot_libpd::MidiRouter midi_router;
	// Guards midi_instances_ and pending_port_errors_.
	//
	// Lock discipline: the MIDI I/O thread holds it only while copying a map
	// entry and pushing one command (non-blocking, unbounded queue), so an
	// instance can never be freed while its pointer is being used —
	// unregister_instance() takes the same lock to erase the entry. The
	// router never calls back into the server while one of its locks is held
	// (midi_router.h contract), so there is no lock-order cycle.
	mutable std::mutex midi_mutex;
	// Registered instances, keyed by instance id (extends the former id-only
	// set). Values are used only on the I/O-thread callback path while
	// midi_mutex is held.
	std::unordered_map<int64_t, LibpdInstance *> midi_instances_;
	// Port errors enqueued by the router's on_port_error callback (I/O thread,
	// or the main-thread open/close timeout path); emitted in
	// _drain_midi_events().
	std::vector<PortErrorEvent> pending_port_errors_;
	// Hotplug port-change events enqueued by the router's on_port_changed
	// callback (I/O thread); emitted in _drain_midi_events().
	std::vector<PortEvent> pending_port_events_;
	// Backs the midi_port_poll_interval property; forwarded to the router.
	double midi_port_poll_interval_ = 0.5;
};

} // namespace godot

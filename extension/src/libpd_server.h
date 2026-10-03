#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pd_event_ring.h"
#include "midi_router.h"

#ifdef NATIVE_AUDIO
#include <memory>

#include "core/audio_port.h"
#include "core/mix_input_ring.h"
#include "core/native_audio.h"
#include "core/portaudio_port.h"
#endif

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

	// ------------------------------------------------------------------
	// Native audio (M5) — the server owns the NativeAudio mix-down.
	// NativeAudio owns the STREAM; the server owns the AudioPort object
	// (invariant #5 from the T4 review: exactly one stream owner, and
	// NativeAudio only borrows the port). #ifndef NATIVE_AUDIO (Android)
	// keeps the API surface with stubs: audio_open() returns false and
	// the instance workers use the Godot generator path.
	// ------------------------------------------------------------------

	/// Open the mix-down stream: p_blocksize frames (multiple of 64) at
	/// p_samplerate; the device gets 0 inputs / p_mix_out outputs, and the
	/// mix-down instance bound via set_mixer() is rendered with p_mix_in
	/// inputs (the SYNTH worker rings) / p_mix_out outputs. False (and
	/// push_error) if already open, the arguments are invalid, or the
	/// stream fails to open.
	bool audio_open(int p_blocksize, int p_samplerate, int p_mix_in = 16, int p_mix_out = 2);
	/// Close the stream (stops the PortAudio callback FIRST, then drops the
	/// mixer binding and rings — the mixer instance is safe to free
	/// afterwards, invariant #1). The mix designation survives; a still-
	/// alive mix instance re-binds on the next audio_open().
	void audio_close();
	/// Designate the mix-down instance. Fails (false + push_error) if audio
	/// is not open, p_mix is null, not the MIXER role, not initialized, its
	/// channel shape differs from the open-time mix shape, or a different
	/// mixer is already bound (Idempotent when p_mix is the current one).
	bool set_mixer(LibpdInstance *p_mix);
	/// Unbind p_instance as the mix-down, under the render lock (invariant
	/// #1) — called from the instance's _exit_tree BEFORE its worker stops,
	/// so the audio thread may still be live.
	void audio_unbind_mixer(LibpdInstance *p_instance);
	/// Run p_fn serialized against the PortAudio callback's mix render
	/// (forwards to NativeAudio::with_mixer_lock; runs p_fn as-is when no
	/// native audio is present). MIXER worker control ops use this.
	template <typename F>
	void audio_with_mixer_lock(F &&p_fn) {
		if (native_audio_ != nullptr) {
			native_audio_->with_mixer_lock(std::forward<F>(p_fn));
		} else {
			p_fn();
		}
	}
	/// True when this build has the native audio path compiled in (false on
	/// Android, where the Godot generator path is used).
	bool audio_available() const;
	/// Output latency in ms of the open stream (0.0 when closed).
	float audio_output_latency_ms();
	/// Audio devices with an output side: {index, name, max_channels}.
	Array audio_list_output_devices();
	/// Audio devices with an input side: {index, name, max_channels}.
	Array audio_list_input_devices();

	/// True when the mix-down stream is open (C++ accessor, unbound).
	bool audio_is_open() const;
	/// Stream blocksize while open (C++ accessor, unbound).
	int audio_blocksize() const;
	/// Stream samplerate while open (C++ accessor, unbound).
	int audio_samplerate() const;

	/// Track a SYNTH instance's ring for re-registration on the next
	/// audio_open(); registers it with NativeAudio right away when open.
	/// (Internal — called from LibpdInstance init/teardown.)
	void audio_register_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring);
	/// Stop tracking p_ring and unregister it from NativeAudio when open.
	/// (Internal — called from LibpdInstance teardown.)
	void audio_unregister_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring);

	/// Test seam (host tests cannot open a real device): audio_open() uses
	/// p_port instead of a PortAudioPort. Must be set while audio is closed.
	void _set_audio_port_for_test(std::unique_ptr<godot_libpd::AudioPort> p_port);

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

#ifdef NATIVE_AUDIO
	// Native audio state (M5). All audio_* calls run on the main thread;
	// audio_mutex additionally guards tracked_rings_ (which the instance
	// init/teardown touch).
	std::unique_ptr<godot_libpd::NativeAudio> native_audio_;
	// Server-owned PortAudio stream (created on first audio_open). NativeAudio
	// only borrows the port — exactly one object owns the stream lifecycle.
	std::unique_ptr<godot_libpd::PortAudioPort> audio_port_;
	// Test seam replacement for audio_port_ (e.g. a NullPort); owned here.
	std::unique_ptr<godot_libpd::AudioPort> audio_test_port_;
	std::mutex audio_mutex;
	bool audio_open_ = false;
	int audio_blocksize_ = 0;
	int audio_samplerate_ = 0;
	// Channel counts as of audio_open() — the mix staging buffers are sized
	// from them, so set_mixer validates against them (invariant #3).
	int open_mix_n_in_ = 16;
	int open_mix_n_out_ = 2;
	// The designated mix-down instance (nullptr = none). Survives audio_close
	// so a still-alive instance re-binds on the next audio_open; nulled by
	// audio_unbind_mixer on the instance's _exit_tree.
	LibpdInstance *mixer_instance_ = nullptr;
	// SYNTH rings to (re)register on audio_open — instance init may happen
	// before the stream opens, and NativeAudio::close() drops all rings.
	std::vector<std::pair<LibpdInstance *, godot_libpd::MixInputRing *>> tracked_rings_;
#endif
};

} // namespace godot

#include "libpd_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>

#include "libpd_instance.h"

using namespace godot;

static LibpdServer *singleton_instance = nullptr;

LibpdServer::LibpdServer() {
	set_process(true);

	// Router callbacks. Both fire on the MIDI I/O thread; on_port_error
	// also fires from the main-thread open/close timeout path. The
	// re-entrancy contract (midi_router.h) forbids calling MidiRouter from
	// either, so we only touch main-thread state under midi_mutex and never
	// emit a signal here — emission happens in _drain_midi_events() on the
	// main thread.
	midi_router.on_midi_command = [this](int64_t p_instance_id, const godot_libpd::PdCommand &p_command) {
		// Copy the instance pointer under midi_mutex and push while still
		// holding it: unregister_instance() takes the same lock to erase
		// the entry, so the pointer is guaranteed alive for the whole push.
		// push_command is non-blocking (unbounded queue), so holding the
		// lock does not stall the I/O thread.
		std::lock_guard<std::mutex> lock(midi_mutex);
		const auto it = midi_instances_.find(p_instance_id);
		if (it == midi_instances_.end()) {
			return; // instance unregistered/freed: drop the command
		}
		it->second->push_midi_command(p_command);
	};
	midi_router.on_port_error = [this](int p_port_id, const char *p_what) {
		// p_what is valid only for the duration of the call — copy it.
		std::lock_guard<std::mutex> lock(midi_mutex);
		pending_port_errors_.emplace_back(PortErrorEvent{p_port_id, String::utf8(p_what)});
	};

	if (singleton_instance != nullptr) {
		// Tolerate it (editor reloads create temporary duplicates) but warn.
		WARN_PRINT("LibpdServer: more than one instance exists; the most recent one is the singleton.");
	}
	singleton_instance = this;
}

LibpdServer::~LibpdServer() {
	// Shutdown order (spec §7): stop routing, then stop the router (close
	// all ports + join the MIDI I/O thread); instance workers tear down
	// independently. By normal exit the instances have already
	// unregistered themselves (_exit_tree -> forget_instance) before the
	// tree frees this autoload; this loop is the safety net.
	std::vector<int64_t> ids;
	{
		std::lock_guard<std::mutex> lock(midi_mutex);
		for (const auto &kv : midi_instances_) {
			ids.push_back(kv.first);
		}
		midi_instances_.clear();
	}
	for (const int64_t id : ids) {
		midi_router.forget_instance(id);
	}
	midi_router.shutdown();

	if (singleton_instance == this) {
		singleton_instance = nullptr;
	}
}

LibpdServer *LibpdServer::get_singleton() {
	return singleton_instance;
}

void LibpdServer::_bind_methods() {
	ADD_SIGNAL(MethodInfo("instance_print", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("instance_note_on", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "pitch"), PropertyInfo(Variant::INT, "velocity")));
	ADD_SIGNAL(MethodInfo("instance_dsp_active", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::BOOL, "active")));

	// MIDI input signals — emitted from _process() (main thread only);
	// 60 Hz granularity, spec §3/§4.
	ADD_SIGNAL(MethodInfo("midi_note_on", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "pitch"), PropertyInfo(Variant::INT, "velocity")));
	ADD_SIGNAL(MethodInfo("midi_note_off", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "pitch"), PropertyInfo(Variant::INT, "velocity")));
	ADD_SIGNAL(MethodInfo("midi_cc", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "controller"), PropertyInfo(Variant::INT, "value")));
	ADD_SIGNAL(MethodInfo("midi_program_change", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "program")));
	ADD_SIGNAL(MethodInfo("midi_pitch_bend", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "value")));
	ADD_SIGNAL(MethodInfo("midi_aftertouch", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "value")));
	ADD_SIGNAL(MethodInfo("midi_poly_aftertouch", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::INT, "pitch"), PropertyInfo(Variant::INT, "value")));
	ADD_SIGNAL(MethodInfo("midi_sysex", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data")));
	ADD_SIGNAL(MethodInfo("midi_port_error", PropertyInfo(Variant::INT, "port_id"), PropertyInfo(Variant::STRING, "what")));

	ClassDB::bind_method(D_METHOD("instance_registered", "instance_id"), &LibpdServer::instance_registered);
	ClassDB::bind_method(D_METHOD("instance_count"), &LibpdServer::instance_count);
	ClassDB::bind_method(D_METHOD("debug_push_print", "instance_id", "text"), &LibpdServer::debug_push_print);

	ClassDB::bind_method(D_METHOD("midi_available"), &LibpdServer::midi_available);
	ClassDB::bind_method(D_METHOD("midi_list_inputs"), &LibpdServer::midi_list_inputs);
	ClassDB::bind_method(D_METHOD("midi_list_outputs"), &LibpdServer::midi_list_outputs);
	ClassDB::bind_method(D_METHOD("midi_open_input", "index"), &LibpdServer::midi_open_input);
	ClassDB::bind_method(D_METHOD("midi_open_output", "index"), &LibpdServer::midi_open_output);
	ClassDB::bind_method(D_METHOD("midi_close_input", "port_id"), &LibpdServer::midi_close_input);
	ClassDB::bind_method(D_METHOD("midi_close_output", "port_id"), &LibpdServer::midi_close_output);
	ClassDB::bind_method(D_METHOD("midi_route_input", "port_id", "instance", "add"), &LibpdServer::midi_route_input, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("midi_route_output", "instance", "port_id", "add"), &LibpdServer::midi_route_output, DEFVAL(true));
}

bool LibpdServer::instance_registered(int64_t p_instance_id) const {
	std::lock_guard<std::mutex> lock(midi_mutex);
	return midi_instances_.count(p_instance_id) > 0;
}

int LibpdServer::instance_count() const {
	std::lock_guard<std::mutex> lock(midi_mutex);
	return (int)midi_instances_.size();
}

void LibpdServer::register_instance(LibpdInstance *p_instance) {
	if (p_instance == nullptr) {
		return;
	}
	const int64_t id = p_instance->instance_id();
	{
		std::lock_guard<std::mutex> lock(midi_mutex);
		midi_instances_[id] = p_instance;
	}
	// Point the router at the instance's output queue (worker thread
	// pushes, MIDI I/O thread drains). Idempotent per instance.
	midi_router.register_instance_output(id, p_instance->midi_output_queue());
}

void LibpdServer::unregister_instance(LibpdInstance *p_instance) {
	if (p_instance == nullptr) {
		return;
	}
	const int64_t id = p_instance->instance_id();
	{
		std::lock_guard<std::mutex> lock(midi_mutex);
		midi_instances_.erase(id);
	}
	// Drop the output-queue registration and every route touching the
	// instance. The worker (and its queue) is still alive here: the
	// instance joins its worker before calling this (see _exit_tree).
	midi_router.forget_instance(id);
}

bool LibpdServer::midi_available() {
	return midi_router.available();
}

Array LibpdServer::midi_list_inputs() {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return Array();
	}
	Array out;
	for (const auto &dev : midi_router.list_inputs()) {
		Dictionary d;
		d["index"] = dev.first;
		d["name"] = String(dev.second.c_str());
		out.push_back(d);
	}
	return out;
}

Array LibpdServer::midi_list_outputs() {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return Array();
	}
	Array out;
	for (const auto &dev : midi_router.list_outputs()) {
		Dictionary d;
		d["index"] = dev.first;
		d["name"] = String(dev.second.c_str());
		out.push_back(d);
	}
	return out;
}

int LibpdServer::midi_open_input(int p_pm_index) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	const int port = midi_router.open_input(p_pm_index);
	if (port < 0) {
		UtilityFunctions::push_error("Failed to open MIDI input port (see the midi_port_error signal)");
	}
	return port;
}

int LibpdServer::midi_open_output(int p_pm_index) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	const int port = midi_router.open_output(p_pm_index);
	if (port < 0) {
		UtilityFunctions::push_error("Failed to open MIDI output port (see the midi_port_error signal)");
	}
	return port;
}

void LibpdServer::midi_close_input(int p_port_id) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return;
	}
	// Read/timeout errors surface via the midi_port_error signal.
	midi_router.close_input(p_port_id);
}

void LibpdServer::midi_close_output(int p_port_id) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return;
	}
	// Timeout errors surface via the midi_port_error signal.
	midi_router.close_output(p_port_id);
}

void LibpdServer::midi_route_input(int p_port_id, LibpdInstance *p_instance, bool p_add) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return;
	}
	if (p_instance == nullptr) {
		UtilityFunctions::push_error("midi_route_input: instance is null");
		return;
	}
	midi_router.route_input(p_port_id, p_instance->instance_id(), p_add);
}

void LibpdServer::midi_route_output(LibpdInstance *p_instance, int p_port_id, bool p_add) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return;
	}
	if (p_instance == nullptr) {
		UtilityFunctions::push_error("midi_route_output: instance is null");
		return;
	}
	midi_router.route_output(p_instance->instance_id(), p_port_id, p_add);
}

void LibpdServer::debug_push_print(int64_t p_instance_id, const String &p_text) {
	godot_libpd::PdEvent e;
	e.instance_id = p_instance_id;
	e.type = godot_libpd::PdEvent::PRINT;
	// Truncate at 63 bytes on a UTF-8 character boundary.
	const CharString bytes = p_text.utf8();
	const uint8_t *data = (const uint8_t *)bytes.get_data();
	size_t len = (size_t)bytes.length() < 63 ? (size_t)bytes.length() : 63;
	while (len > 0 && (data[len] & 0xC0) == 0x80) { // back off continuation bytes
		len--;
	}
	memcpy(e.data, data, len);
	e.data[len] = '\0';
	ring.push(e);
}

void LibpdServer::push_event(const godot_libpd::PdEvent &p_event) {
	ring.push(p_event);
}

void LibpdServer::_process(double p_delta) {
	(void)p_delta;
	_drain_ring();
	_drain_midi_events();
}

void LibpdServer::_drain_ring() {
	// Pop one event at a time and emit immediately, bounded per frame so a print
	// flood can't starve the main thread (the ring itself drops the oldest on
	// overflow — see PdEventRing). A batch pop into a large stack buffer followed
	// by a tight emit loop dropped signal deliveries under load (Task 6); this
	// form keeps each emit independent and avoids the large stack allocation.
	constexpr int MAX_PER_FRAME = 128;
	godot_libpd::PdEvent e;
	int processed = 0;
	while (processed < MAX_PER_FRAME && ring.pop(&e, 1) == 1) {
		switch (e.type) {
			case godot_libpd::PdEvent::PRINT:
				emit_signal("instance_print", (int64_t)e.instance_id, String(e.data));
				break;
			case godot_libpd::PdEvent::NOTE_ON:
				emit_signal("instance_note_on", (int64_t)e.instance_id,
						(int)e.data[0], (int)(unsigned char)e.data[1], (int)(unsigned char)e.data[2]);
				break;
			case godot_libpd::PdEvent::DSP_ACTIVE:
				emit_signal("instance_dsp_active", (int64_t)e.instance_id, e.data[0] != 0);
				break;
		}
		processed++;
	}
}

void LibpdServer::_drain_midi_events() {
	// Decoded input events: the router's MIDI I/O thread appends them to a
	// bounded ring (drop-oldest backpressure); this is the only place the
	// typed signals fire, so no Godot call ever runs off the main thread.
	std::vector<godot_libpd::MidiSignalEvent> events;
	midi_router.drain_signal_events(events);
	for (const godot_libpd::MidiSignalEvent &ev : events) {
		if (ev.is_sysex) {
			// The stored body is F0-exclusive, F7-inclusive (framer
			// contract); emit the full F0..F7 sequence — the same bytes pd's
			// [sysexin] receives via the MIDI_SYSEX command.
			PackedByteArray data;
			data.resize((int64_t)ev.sysex.size() + 1);
			uint8_t *dst = data.ptrw();
			dst[0] = 0xF0;
			std::copy(ev.sysex.begin(), ev.sysex.end(), dst + 1);
			emit_signal("midi_sysex", ev.port_id, data);
			continue;
		}
		const godot_libpd::MidiShortMsg &m = ev.msg;
		const int port = ev.port_id;
		const int ch = m.channel;
		const int d1 = m.d1;
		const int d2 = m.d2;
		switch (m.kind) {
			case godot_libpd::MidiKind::NOTE_ON:
				emit_signal("midi_note_on", port, ch, d1, d2);
				break;
			case godot_libpd::MidiKind::NOTE_OFF:
				emit_signal("midi_note_off", port, ch, d1, d2);
				break;
			case godot_libpd::MidiKind::CC:
				emit_signal("midi_cc", port, ch, d1, d2);
				break;
			case godot_libpd::MidiKind::PROGRAM_CHANGE:
				emit_signal("midi_program_change", port, ch, d1);
				break;
			case godot_libpd::MidiKind::PITCH_BEND:
				// 14-bit value 0..16383: d1 = low 7 bits, d2 = high 7 bits.
				emit_signal("midi_pitch_bend", port, ch, d1 + (d2 << 7));
				break;
			case godot_libpd::MidiKind::AFTERTOUCH:
				emit_signal("midi_aftertouch", port, ch, d1);
				break;
			case godot_libpd::MidiKind::POLY_AFTERTOUCH:
				emit_signal("midi_poly_aftertouch", port, ch, d1, d2);
				break;
		}
	}

	// Port errors enqueued by the router's on_port_error callback (I/O
	// thread, or the main-thread open/close timeout path). Emitted here,
	// never from the callback itself (main-thread-only signals).
	std::vector<PortErrorEvent> errors;
	{
		std::lock_guard<std::mutex> lock(midi_mutex);
		errors.swap(pending_port_errors_);
	}
	for (const PortErrorEvent &err : errors) {
		emit_signal("midi_port_error", err.port_id, err.what);
	}
}


#include "libpd_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

static LibpdServer *singleton_instance = nullptr;

LibpdServer::LibpdServer() {
	set_process(true);
	if (singleton_instance != nullptr) {
		// Tolerate it (editor reloads create temporary duplicates) but warn.
		WARN_PRINT("LibpdServer: more than one instance exists; the most recent one is the singleton.");
	}
	singleton_instance = this;
}

LibpdServer::~LibpdServer() {
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

	ClassDB::bind_method(D_METHOD("instance_registered", "instance_id"), &LibpdServer::instance_registered);
	ClassDB::bind_method(D_METHOD("instance_count"), &LibpdServer::instance_count);
	ClassDB::bind_method(D_METHOD("debug_push_print", "instance_id", "text"), &LibpdServer::debug_push_print);
}

bool LibpdServer::instance_registered(int64_t p_instance_id) const {
	return instances.count(p_instance_id) > 0;
}

int LibpdServer::instance_count() const {
	return (int)instances.size();
}

void LibpdServer::register_instance(int64_t p_instance_id) {
	instances.insert(p_instance_id);
}

void LibpdServer::unregister_instance(int64_t p_instance_id) {
	instances.erase(p_instance_id);
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

void LibpdServer::_process(double p_delta) {
	(void)p_delta;
	_drain_ring();
}

void LibpdServer::_drain_ring() {
	// Bounded per frame so a print flood can't starve the main thread;
	// the ring drops the oldest on overflow (see PdEventRing).
	godot_libpd::PdEvent events[128];
	for (size_t i = 0; i < ring.pop(events, 128); i++) {
		const godot_libpd::PdEvent &e = events[i];
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
	}
}


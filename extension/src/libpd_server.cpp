#include "libpd_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>

#include "libpd_instance.h"

#ifdef NATIVE_AUDIO
#ifdef __ANDROID__
#include "core/opensles_port.h"
#endif
#endif

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
	// Hotplug (M4): port add/remove from the router's I/O thread. Copy into
	// pending_port_events_ under midi_mutex; emission happens in
	// _drain_midi_events() on the main thread (never here).
	midi_router.on_port_changed = [this](bool p_added, const char *p_kind, int p_index, const char *p_name) {
		std::lock_guard<std::mutex> lock(midi_mutex);
		pending_port_events_.push_back(PortEvent{p_added, String(p_kind), p_index, String(p_name)});
	};
	// Trigger the initial re-enumeration now that the callbacks are wired, so
	// the annotated-on-start baseline (midi_port_added for every device present
	// at init) fires deterministically and immediately, independent of the
	// poll interval. (The router's first periodic tick is already deferred one
	// interval; this makes the startup annotation happen right away.)
	midi_router.refresh_ports();

	if (singleton_instance != nullptr) {
		// Tolerate it (editor reloads create temporary duplicates) but warn.
		WARN_PRINT("LibpdServer: more than one instance exists; the most recent one is the singleton.");
	}
	singleton_instance = this;
}

LibpdServer::~LibpdServer() {
	// Native audio first (M5): stop the PortAudio callback and drop the mix
	// binding before anything else goes. By normal exit the mix instance has
	// already unbound itself (_exit_tree -> audio_unbind_mixer); this only
	// joins the audio thread and clears state — it never frees the instance.
	audio_close();

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
	ADD_SIGNAL(MethodInfo("instance_bang", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::STRING, "receiver")));
	ADD_SIGNAL(MethodInfo("instance_float", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::STRING, "receiver"), PropertyInfo(Variant::FLOAT, "value")));
	ADD_SIGNAL(MethodInfo("instance_symbol", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::STRING, "receiver"), PropertyInfo(Variant::STRING, "symbol")));
	ADD_SIGNAL(MethodInfo("instance_list", PropertyInfo(Variant::INT, "instance_id"), PropertyInfo(Variant::STRING, "receiver"), PropertyInfo(Variant::ARRAY, "items")));

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
	// Hotplug (M4): live device add/remove. `kind` is "input"/"output"; index
	// is the current index (added) or the last remembered index (removed).
	ADD_SIGNAL(MethodInfo("midi_port_added", PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::INT, "index"), PropertyInfo(Variant::STRING, "name")));
	ADD_SIGNAL(MethodInfo("midi_port_removed", PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::INT, "index"), PropertyInfo(Variant::STRING, "name")));

	ClassDB::bind_method(D_METHOD("instance_registered", "instance_id"), &LibpdServer::instance_registered);
	ClassDB::bind_method(D_METHOD("instance_count"), &LibpdServer::instance_count);
	ClassDB::bind_method(D_METHOD("debug_push_print", "instance_id", "text"), &LibpdServer::debug_push_print);

	ClassDB::bind_method(D_METHOD("midi_available"), &LibpdServer::midi_available);
	ClassDB::bind_method(D_METHOD("midi_list_inputs"), &LibpdServer::midi_list_inputs);
	ClassDB::bind_method(D_METHOD("midi_list_outputs"), &LibpdServer::midi_list_outputs);
	ClassDB::bind_method(D_METHOD("midi_open_input", "index"), &LibpdServer::midi_open_input);
	ClassDB::bind_method(D_METHOD("midi_open_output", "index"), &LibpdServer::midi_open_output);
	ClassDB::bind_method(D_METHOD("midi_open_virtual_input"), &LibpdServer::midi_open_virtual_input);
	ClassDB::bind_method(D_METHOD("midi_open_virtual_output"), &LibpdServer::midi_open_virtual_output);
	ClassDB::bind_method(D_METHOD("midi_create_loopback", "name"), &LibpdServer::midi_create_loopback);
	ClassDB::bind_method(D_METHOD("midi_close_input", "port_id"), &LibpdServer::midi_close_input);
	ClassDB::bind_method(D_METHOD("midi_close_output", "port_id"), &LibpdServer::midi_close_output);
	ClassDB::bind_method(D_METHOD("midi_route_input", "port_id", "instance", "add"), &LibpdServer::midi_route_input, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("midi_route_output", "instance", "port_id", "add"), &LibpdServer::midi_route_output, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("midi_refresh_ports"), &LibpdServer::midi_refresh_ports);
	ClassDB::bind_method(D_METHOD("midi_set_poll_interval", "seconds"), &LibpdServer::midi_set_poll_interval);
	ClassDB::bind_method(D_METHOD("midi_get_poll_interval"), &LibpdServer::midi_get_poll_interval);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midi_port_poll_interval"), "midi_set_poll_interval", "midi_get_poll_interval");

	// Native audio (M5). Bound in both builds: on Android (no NATIVE_AUDIO)
	// the implementations are stubs (audio_open -> false, lists -> empty),
	// so the GDScript surface is stable across platforms.
	ClassDB::bind_method(D_METHOD("audio_open", "blocksize", "samplerate", "mix_in", "mix_out"), &LibpdServer::audio_open, DEFVAL(16), DEFVAL(2));
	ClassDB::bind_method(D_METHOD("audio_close"), &LibpdServer::audio_close);
	ClassDB::bind_method(D_METHOD("set_mixer", "mix"), &LibpdServer::set_mixer);
	ClassDB::bind_method(D_METHOD("audio_list_output_devices"), &LibpdServer::audio_list_output_devices);
	ClassDB::bind_method(D_METHOD("audio_list_input_devices"), &LibpdServer::audio_list_input_devices);
	ClassDB::bind_method(D_METHOD("audio_output_latency_ms"), &LibpdServer::audio_output_latency_ms);
	ClassDB::bind_method(D_METHOD("audio_available"), &LibpdServer::audio_available);
}

#ifdef NATIVE_AUDIO

bool LibpdServer::audio_open(int p_blocksize, int p_samplerate, int p_mix_in, int p_mix_out) {
	if (audio_open_) {
		UtilityFunctions::push_error("audio_open: audio is already open");
		return false;
	}
	// NativeAudio::open enforces the same contract on the port side; these
	// checks fail fast on the control side with a precise message.
	if (p_blocksize <= 0 || p_blocksize % 64 != 0) {
		UtilityFunctions::push_error(vformat("audio_open: blocksize %d must be a positive multiple of 64 (libpd processes in %d-frame ticks)", p_blocksize, 64));
		return false;
	}
	if (p_samplerate <= 0 || p_mix_in < 0 || p_mix_out <= 0) {
		UtilityFunctions::push_error(vformat("audio_open: invalid arguments (samplerate=%d, mix_in=%d, mix_out=%d)", p_samplerate, p_mix_in, p_mix_out));
		return false;
	}
#ifdef __ANDROID__
	// OpenSL ES buffer-queue delivery is smooth only at ≥1024-frame buffers
	// (M6' T2 device measurement: 256-frame buffers ~26% late). The port has
	// a fixed 1024-frame buffer; round the stream blocksize up to match it.
	if (p_blocksize < godot_libpd::OpenSLESPort::frames_per_buffer()) {
		p_blocksize = godot_libpd::OpenSLESPort::frames_per_buffer();
	}
#endif
	if (native_audio_ == nullptr) {
		native_audio_ = std::make_unique<godot_libpd::NativeAudio>();
	}
	// Port ownership: the server owns the AudioPort object; NativeAudio only
	// borrows it (invariant #5). A test port set via _set_audio_port_for_test
	// (host tests: NullPort) takes precedence.
	godot_libpd::AudioPort *port = nullptr;
	if (audio_test_port_ != nullptr) {
		port = audio_test_port_.get();
	} else {
		if (audio_port_ == nullptr) {
			audio_port_ = godot_libpd::create_platform_port();
		}
		port = audio_port_.get();
	}
	if (!native_audio_->open(port, p_mix_in, p_mix_out, p_blocksize, p_samplerate)) {
		UtilityFunctions::push_error(vformat("audio_open: failed to open the audio stream (%d frames @ %d Hz)", p_blocksize, p_samplerate));
		return false;
	}
	audio_open_ = true;
	audio_blocksize_ = p_blocksize;
	audio_samplerate_ = p_samplerate;
	open_mix_n_in_ = p_mix_in;
	open_mix_n_out_ = p_mix_out;
	// Re-register rings tracked from instance inits that happened BEFORE the
	// stream opened (NativeAudio::open() starts with no rings), then restore
	// the mixer binding for a still-alive mix instance.
	std::vector<godot_libpd::MixInputRing *> rings;
	{
		std::lock_guard<std::mutex> lock(audio_mutex);
		rings.reserve(tracked_rings_.size());
		for (const auto &entry : tracked_rings_) {
			rings.push_back(entry.second);
		}
	}
	for (godot_libpd::MixInputRing *ring : rings) {
		native_audio_->register_worker_ring(ring);
	}
	if (mixer_instance_ != nullptr && mixer_instance_->pd_instance_ptr() != nullptr) {
		native_audio_->set_mixer(mixer_instance_->pd_instance_ptr(), open_mix_n_in_, open_mix_n_out_);
	}
	return true;
}

void LibpdServer::audio_close() {
	if (!audio_open_ || native_audio_ == nullptr) {
		return;
	}
	// Invariant #1 (T4 review): close() stops the PortAudio callback (joins
	// the audio thread) BEFORE dropping the mixer binding and rings — the
	// mix instance is safe to free after this returns. The designation
	// (mixer_instance_) survives so the instance re-binds on the next open.
	native_audio_->close();
	audio_open_ = false;
	audio_blocksize_ = 0;
}

bool LibpdServer::set_mixer(LibpdInstance *p_mix) {
	if (p_mix == nullptr) {
		UtilityFunctions::push_error("set_mixer: instance is null");
		return false;
	}
	if (!audio_open_) {
		UtilityFunctions::push_error("set_mixer: audio is not open (audio_open first)");
		return false;
	}
	if (p_mix->get_role() != LibpdInstance::Role::MIXER) {
		UtilityFunctions::push_error("set_mixer: instance is not the MIXER role (set_role before init)");
		return false;
	}
	if (p_mix->pd_instance_ptr() == nullptr) {
		UtilityFunctions::push_error("set_mixer: instance is not initialized (init before set_mixer)");
		return false;
	}
	// Invariant #3 (T4 review): the mix staging buffers are sized from the
	// open-time shape — a differently shaped mix instance would overflow them.
	if (p_mix->init_n_ins() != open_mix_n_in_ || p_mix->init_n_out() != open_mix_n_out_) {
		UtilityFunctions::push_error(vformat("set_mixer: instance shape (%d in / %d out) != open-time mix shape (%d in / %d out)", p_mix->init_n_ins(), p_mix->init_n_out(), open_mix_n_in_, open_mix_n_out_));
		return false;
	}
	if (mixer_instance_ == p_mix) {
		return true; // already designated — idempotent
	}
	if (mixer_instance_ != nullptr) {
		UtilityFunctions::push_error("set_mixer: a different mixer is already bound (the old mix instance must be unbound/freed first)");
		return false;
	}
	if (!native_audio_->set_mixer(p_mix->pd_instance_ptr(), open_mix_n_in_, open_mix_n_out_)) {
		UtilityFunctions::push_error("set_mixer: NativeAudio rejected the binding (see its validation contract)");
		return false;
	}
	mixer_instance_ = p_mix;
	return true;
}

void LibpdServer::audio_unbind_mixer(LibpdInstance *p_instance) {
	if (p_instance == nullptr || mixer_instance_ != p_instance) {
		return;
	}
	// Invariant #1: unbind UNDER the render lock before the instance stops
	// its worker — the audio thread may still be live.
	if (native_audio_ != nullptr) {
		native_audio_->clear_mixer();
	}
	mixer_instance_ = nullptr;
}

bool LibpdServer::audio_available() const {
	return true;
}

float LibpdServer::audio_output_latency_ms() {
	if (!audio_open_ || native_audio_ == nullptr) {
		return 0.0f;
	}
	const godot_libpd::AudioPort *port = native_audio_->port();
	return port != nullptr ? (float)port->output_latency_ms() : 0.0f;
}

Array LibpdServer::audio_list_output_devices() {
	// Listing works on a closed stream, so fall back to a probe port when the
	// stream is not open (the test port, else a fresh platform port).
	godot_libpd::AudioPort *port = nullptr;
	if (audio_open_ && native_audio_ != nullptr && native_audio_->port() != nullptr) {
		port = native_audio_->port();
	} else if (audio_test_port_ != nullptr) {
		port = audio_test_port_.get();
	} else {
		if (audio_port_ == nullptr) {
			audio_port_ = godot_libpd::create_platform_port();
		}
		port = audio_port_.get();
	}
	Array out;
	for (const auto &dev : port->list_outputs()) {
		Dictionary d;
		d["index"] = dev.index;
		d["name"] = String(dev.name.c_str());
		d["max_channels"] = dev.max_out;
		out.push_back(d);
	}
	return out;
}

Array LibpdServer::audio_list_input_devices() {
	godot_libpd::AudioPort *port = nullptr;
	if (audio_open_ && native_audio_ != nullptr && native_audio_->port() != nullptr) {
		port = native_audio_->port();
	} else if (audio_test_port_ != nullptr) {
		port = audio_test_port_.get();
	} else {
		if (audio_port_ == nullptr) {
			audio_port_ = godot_libpd::create_platform_port();
		}
		port = audio_port_.get();
	}
	Array out;
	for (const auto &dev : port->list_inputs()) {
		Dictionary d;
		d["index"] = dev.index;
		d["name"] = String(dev.name.c_str());
		d["max_channels"] = dev.max_in;
		out.push_back(d);
	}
	return out;
}

bool LibpdServer::audio_is_open() const {
	return audio_open_;
}

int LibpdServer::audio_blocksize() const {
	return audio_blocksize_;
}

int LibpdServer::audio_samplerate() const {
	return audio_samplerate_;
}

void LibpdServer::audio_register_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring) {
	(void)p_instance;
	if (p_ring == nullptr) {
		return;
	}
	// Track for the re-open path (audio_close drops all registered rings),
	// then register right away when the stream is live.
	{
		std::lock_guard<std::mutex> lock(audio_mutex);
		for (const auto &entry : tracked_rings_) {
			if (entry.second == p_ring) {
				return; // already tracked (defensive: one ring per instance)
			}
		}
		tracked_rings_.push_back({p_instance, p_ring});
	}
	if (audio_open_ && native_audio_ != nullptr) {
		native_audio_->register_worker_ring(p_ring);
	}
}

void LibpdServer::audio_unregister_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring) {
	(void)p_instance;
	if (p_ring == nullptr) {
		return;
	}
	if (audio_open_ && native_audio_ != nullptr) {
		native_audio_->unregister_worker_ring(p_ring);
	}
	std::lock_guard<std::mutex> lock(audio_mutex);
	tracked_rings_.erase(std::remove_if(tracked_rings_.begin(), tracked_rings_.end(),
			[p_ring](const auto &entry) { return entry.second == p_ring; }),
		tracked_rings_.end());
}

void LibpdServer::_set_audio_port_for_test(std::unique_ptr<godot_libpd::AudioPort> p_port) {
	if (audio_open_) {
		UtilityFunctions::push_error("_set_audio_port_for_test: audio is open — close it first");
		return;
	}
	audio_test_port_ = std::move(p_port);
}

#else // !NATIVE_AUDIO (Android: Godot generator path only)

bool LibpdServer::audio_open(int p_blocksize, int p_samplerate, int p_mix_in, int p_mix_out) {
	(void)p_blocksize;
	(void)p_samplerate;
	(void)p_mix_in;
	(void)p_mix_out;
	UtilityFunctions::push_error("audio_open: native audio is not available in this build");
	return false;
}

void LibpdServer::audio_close() {
	// No native stream on this platform.
}

bool LibpdServer::set_mixer(LibpdInstance *p_mix) {
	(void)p_mix;
	UtilityFunctions::push_error("set_mixer: native audio is not available in this build");
	return false;
}

void LibpdServer::audio_unbind_mixer(LibpdInstance *p_instance) {
	(void)p_instance;
}

bool LibpdServer::audio_available() const {
	return false;
}

float LibpdServer::audio_output_latency_ms() {
	return 0.0f;
}

Array LibpdServer::audio_list_output_devices() {
	UtilityFunctions::push_error("audio_list_output_devices: native audio is not available in this build");
	return Array();
}

Array LibpdServer::audio_list_input_devices() {
	UtilityFunctions::push_error("audio_list_input_devices: native audio is not available in this build");
	return Array();
}

bool LibpdServer::audio_is_open() const {
	return false;
}

int LibpdServer::audio_blocksize() const {
	return 0;
}

int LibpdServer::audio_samplerate() const {
	return 0;
}

void LibpdServer::audio_register_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring) {
	(void)p_instance;
	(void)p_ring;
}

void LibpdServer::audio_unregister_ring(LibpdInstance *p_instance, godot_libpd::MixInputRing *p_ring) {
	(void)p_instance;
	(void)p_ring;
}

void LibpdServer::_set_audio_port_for_test(std::unique_ptr<godot_libpd::AudioPort> p_port) {
	(void)p_port;
}

#endif // NATIVE_AUDIO

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

int LibpdServer::midi_open_virtual_input() {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	const int port = midi_router.open_virtual_input();
	if (port < 0) {
		UtilityFunctions::push_error(
				"Failed to open virtual MIDI input port (see the midi_port_error signal)");
	}
	return port;
}

int LibpdServer::midi_open_virtual_output() {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	const int port = midi_router.open_virtual_output();
	if (port < 0) {
		UtilityFunctions::push_error(
				"Failed to open virtual MIDI output port (see the midi_port_error signal)");
	}
	return port;
}

int LibpdServer::midi_create_loopback(const String &p_name) {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	const int err = midi_router.create_virtual_loopback(p_name.utf8().get_data());
	if (err != 0) {
		UtilityFunctions::push_error(
				"Failed to create the in-process MIDI loopback (see the "
				"midi_port_error signal); it is unavailable on the PortMidi "
				"hosts — use IAC / aconnect there");
		return -1;
	}
	return 0;
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

int LibpdServer::midi_refresh_ports() {
	if (!midi_available()) {
		UtilityFunctions::push_error("MIDI not available on this platform");
		return -1;
	}
	// -1 from the router means no I/O thread / timeout (surfaced via
	// midi_port_error with port_id -1).
	return midi_router.refresh_ports();
}

void LibpdServer::midi_set_poll_interval(double p_seconds) {
	if (p_seconds < 0.01) {
		p_seconds = 0.01; // avoid a busy re-enumeration loop
	}
	midi_port_poll_interval_ = p_seconds;
	midi_router.set_poll_interval(p_seconds);
}

double LibpdServer::midi_get_poll_interval() const {
	return midi_port_poll_interval_;
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
			case godot_libpd::PdEvent::BANG:
				emit_signal("instance_bang", (int64_t)e.instance_id, String(e.data));
				break;
			case godot_libpd::PdEvent::FLOAT:
				emit_signal("instance_float", (int64_t)e.instance_id, String(e.data), (double)e.fval);
				break;
			case godot_libpd::PdEvent::SYMBOL:
				emit_signal("instance_symbol", (int64_t)e.instance_id, String(e.data), String(e.sval));
				break;
			case godot_libpd::PdEvent::LIST: {
				// Mixed Array: float items as float64, symbol items as String
				// (the Godot value type IS the type identifier; spec ruling 2).
				Array items;
				items.resize((int64_t)e.n_items);
				for (uint32_t i = 0; i < e.n_items && i < (uint32_t)godot_libpd::PdEvent::k_max_list_items; i++) {
					if (e.list_is_symbol[i]) {
						items[i] = String(e.list_syms[i]);
					} else {
						items[i] = (double)e.list_floats[i];
					}
				}
				emit_signal("instance_list", (int64_t)e.instance_id, String(e.data), items);
				break;
			}
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

	// Hotplug port-change events enqueued by the router's on_port_changed
	// callback (I/O thread); emitted here (main-thread-only signals).
	std::vector<PortEvent> port_events;
	{
		std::lock_guard<std::mutex> lock(midi_mutex);
		port_events.swap(pending_port_events_);
	}
	for (const PortEvent &pe : port_events) {
		emit_signal(pe.added ? "midi_port_added" : "midi_port_removed",
				pe.kind, pe.index, pe.name);
	}
}


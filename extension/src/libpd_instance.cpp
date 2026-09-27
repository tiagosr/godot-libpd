#include "libpd_instance.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdio>
#include <future>
#include <mutex>

#include "z_libpd.h"

#include "libpd_server.h"

using namespace godot;

int64_t LibpdInstance::_next_instance_id = 0;

namespace {

typedef std::shared_ptr<std::promise<int>> IntPromise;

// Helper: run a worker command that fulfils a promise, and wait (bounded)
// for its result. p_push receives the promise slot to attach to the command.
// The shared_ptr stays alive for the whole wait, so the worker can fulfil it.
template <typename Push>
int wait_for_command(Push p_push, int64_t p_timeout_ms = 5000) {
	IntPromise promise = std::make_shared<std::promise<int>>();
	std::future<int> future = promise->get_future();
	p_push(&promise); // p_push(IntPromise *)
	if (future.wait_for(std::chrono::milliseconds(p_timeout_ms)) != std::future_status::ready) {
		return -1; // timeout
	}
	return future.get();
}

} // namespace

LibpdInstance::LibpdInstance() :
		worker([this] {
			godot_libpd::LibpdWorker::Config cfg;
			cfg.instance_id = ++_next_instance_id;
			cfg.samplerate = 44100;
			cfg.n_ins = 0;
			cfg.n_out = 2;
			// Note: dry_sink is a later member; taking its address here is safe
			// because the worker only dereferences it during processing (post-construction).
			cfg.sink = &dry_sink;
			cfg.on_event = [](const godot_libpd::PdEvent &e) {
				if (LibpdServer::get_singleton() != nullptr) {
					LibpdServer::get_singleton()->push_event(e);
				}
			};
			return cfg;
		}()) {
	// dry_sink is the default sink; replaced by GeneratorSink in Task 5.
}

LibpdInstance::~LibpdInstance() {
	worker.request_stop();
	worker.join();
}

void LibpdInstance::_bind_methods() {
	ADD_SIGNAL(MethodInfo("failure", PropertyInfo(Variant::INT, "code"), PropertyInfo(Variant::STRING, "text")));

	ClassDB::bind_method(D_METHOD("init", "samplerate", "n_ins", "n_out"), &LibpdInstance::init, DEFVAL(44100), DEFVAL(0), DEFVAL(2));
	ClassDB::bind_method(D_METHOD("load_patch", "path", "search_paths"), &LibpdInstance::load_patch, DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("unload_patch"), &LibpdInstance::unload_patch);
	ClassDB::bind_method(D_METHOD("start_dsp"), &LibpdInstance::start_dsp);
	ClassDB::bind_method(D_METHOD("stop_dsp"), &LibpdInstance::stop_dsp);
	ClassDB::bind_method(D_METHOD("send_pd_message", "receiver", "args"), &LibpdInstance::send_pd_message);
	ClassDB::bind_method(D_METHOD("set_parameter", "path", "value"), &LibpdInstance::set_parameter);
	ClassDB::bind_method(D_METHOD("send_midi", "channel", "pitch", "velocity"), &LibpdInstance::send_midi);

	ClassDB::bind_method(D_METHOD("get_instance_id"), &LibpdInstance::instance_id);
	ClassDB::bind_method(D_METHOD("get_samplerate"), &LibpdInstance::samplerate);
	ClassDB::bind_method(D_METHOD("is_patch_loaded"), &LibpdInstance::patch_loaded);
	ClassDB::bind_method(D_METHOD("is_dsp_active"), &LibpdInstance::dsp_active);
	ClassDB::bind_method(D_METHOD("get_debug_blocks_pushed"), &LibpdInstance::debug_blocks_pushed);
	ClassDB::bind_method(D_METHOD("get_debug_sink_peak"), &LibpdInstance::debug_sink_peak);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "instance_id"), "", "get_instance_id");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "samplerate"), "", "get_samplerate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "patch_loaded"), "", "is_patch_loaded");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dsp_active"), "", "is_dsp_active");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_blocks_pushed"), "", "get_debug_blocks_pushed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_sink_peak"), "", "get_debug_sink_peak");
}

void LibpdInstance::_enter_tree() {
	if (LibpdServer::get_singleton() != nullptr) {
		LibpdServer::get_singleton()->register_instance(instance_id_value = worker.instance_id());
	} else {
		instance_id_value = worker.instance_id();
	}
}

void LibpdInstance::_exit_tree() {
	// Synchronous teardown (spec §5): stop, join, then the pd instance is
	// freed on the worker thread.
	if (worker.is_running()) {
		worker.request_stop();
		worker.join();
	}
	if (LibpdServer::get_singleton() != nullptr) {
		LibpdServer::get_singleton()->unregister_instance(worker.instance_id());
	}
}

bool LibpdInstance::init(int p_samplerate, int p_n_ins, int p_n_out) {
	if (initialized.load()) {
		return false; // double init
	}
	samplerate_value = p_samplerate;

	worker.start();

	godot_libpd::PdCommand cmd;
	cmd.opcode = godot_libpd::PdCommand::INIT;
	cmd.i32 = p_samplerate;
	cmd.i64 = (int64_t)(p_n_ins * 1000 + p_n_out);
	const int res = wait_for_command([&cmd, this](std::shared_ptr<std::promise<int>> *slot) {
		cmd.result = slot;
		worker.push_command(cmd);
	});

	if (res != 0) {
		_emit_failure(-1, "libpd initialization failed");
		return false;
	}
	initialized = true;
	return true;
}

String LibpdInstance::resolve_patch_path(const String &p_path, bool *r_ok) {
	*r_ok = false;
	if (p_path.begins_with("res://")) {
		const String abs = ProjectSettings::get_singleton()->globalize_path(p_path);
		if (FileAccess::file_exists(abs)) {
			*r_ok = true;
			return abs;
		}
		// Pack-only resource: copy it out so the C library can open it.
		Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
		if (!f.is_valid()) {
			return String();
		}
		const PackedByteArray bytes = f->get_buffer((uint64_t)f->get_length());
		const String user_dir = OS::get_singleton()->get_user_data_dir();
		DirAccess::make_dir_recursive_absolute(user_dir.path_join("godot_libpd"));
		const String out = user_dir.path_join("godot_libpd").path_join(p_path.get_file());
		Ref<FileAccess> of = FileAccess::open(out, FileAccess::WRITE);
		if (of.is_valid()) {
			of->store_buffer(bytes);
			of->close();
			*r_ok = true;
			return out;
		}
		return String();
	}
	// Absolute or relative filesystem path.
	if (p_path.begins_with("/")) {
		if (FileAccess::file_exists(p_path)) {
			*r_ok = true;
			return p_path;
		}
		return String();
	}
	Ref<DirAccess> da = DirAccess::open(".");
		const String cwd = da.is_valid() ? da->get_current_dir() : String();
	const String abs = cwd.path_join(p_path);
	if (FileAccess::file_exists(abs)) {
		*r_ok = true;
		return abs;
	}
	return String();
}

int LibpdInstance::load_patch(const String &p_path, const PackedStringArray &p_search_paths) {
	if (!initialized.load()) {
		_emit_failure(-1, "load_patch before init");
		return (int)Error::ERR_INVALID_DATA;
	}
	bool ok = false;
	const String resolved = resolve_patch_path(p_path, &ok);
	if (!ok) {
		_emit_failure((int)Error::ERR_FILE_NOT_FOUND, "patch not found: " + p_path);
		return (int)Error::ERR_FILE_NOT_FOUND;
	}

	String search = "";
	for (int i = 0; i < p_search_paths.size(); i++) {
		const String s = p_search_paths[i];
		const String abs = s.begins_with("res://") ? ProjectSettings::get_singleton()->globalize_path(s) : s;
		search = search.is_empty() ? abs : search + String("\n") + abs;
	}

	godot_libpd::PdCommand cmd;
	cmd.opcode = godot_libpd::PdCommand::LOAD;
	cmd.path = resolved.utf8().get_data();
	cmd.search = search.utf8().get_data();
	const int res = wait_for_command([&cmd, this](std::shared_ptr<std::promise<int>> *slot) {
		cmd.result = slot;
		worker.push_command(cmd);
	});

	if (res != 0) {
		_emit_failure((int)Error::ERR_FILE_NOT_FOUND, "failed to open patch: " + p_path);
		return (int)Error::ERR_FILE_NOT_FOUND;
	}
	has_patch = true;
	return (int)Error::OK;
}

int LibpdInstance::unload_patch() {
	if (!initialized.load()) {
		return (int)Error::ERR_INVALID_DATA;
	}
	if (dsp_running.load()) {
		stop_dsp();
	}
	godot_libpd::PdCommand cmd;
	cmd.opcode = godot_libpd::PdCommand::UNLOAD;
	const int res = wait_for_command([&cmd, this](std::shared_ptr<std::promise<int>> *slot) {
		cmd.result = slot;
		worker.push_command(cmd);
	});
	has_patch = (res == 0);
	return res == 0 ? (int)Error::OK : (int)Error::ERR_INVALID_DATA;
}

int LibpdInstance::start_dsp() {
	if (!initialized.load()) {
		_emit_failure(-1, "start_dsp before init");
		return (int)Error::ERR_INVALID_DATA;
	}
	worker.set_dsp(true);
	dsp_running = true;
	if (LibpdServer::get_singleton() != nullptr) {
		godot_libpd::PdEvent e;
		e.instance_id = worker.instance_id();
		e.type = godot_libpd::PdEvent::DSP_ACTIVE;
		e.data[0] = 1;
		LibpdServer::get_singleton()->push_event(e);
	}
	return (int)Error::OK;
}

int LibpdInstance::stop_dsp() {
	if (!initialized.load()) {
		return (int)Error::ERR_INVALID_DATA;
	}
	worker.set_dsp(false);
	dsp_running = false;
	if (LibpdServer::get_singleton() != nullptr) {
		godot_libpd::PdEvent e;
		e.instance_id = worker.instance_id();
		e.type = godot_libpd::PdEvent::DSP_ACTIVE;
		e.data[0] = 0;
		LibpdServer::get_singleton()->push_event(e);
	}
	return (int)Error::OK;
}

void LibpdInstance::send_pd_message(const String &p_receiver, const PackedStringArray &p_args) {
	if (!worker.is_running()) {
		return;
	}
	String msg;
	for (int i = 0; i < p_args.size(); i++) {
		msg += (i == 0 ? "" : " ") + p_args[i];
	}
	godot_libpd::PdCommand cmd;
	cmd.opcode = godot_libpd::PdCommand::MESSAGE;
	cmd.path = p_receiver.utf8().get_data();
	cmd.args = msg.utf8().get_data();
	worker.push_command(cmd);
}

void LibpdInstance::set_parameter(const String &p_path, double p_value) {
	// v1: send the float as a string arg; pd parses numerics as floats.
	send_pd_message(p_path, { String::num_real(p_value, 9) });
}

void LibpdInstance::send_midi(int p_channel, int p_pitch, int p_velocity) {
	if (!worker.is_running()) {
		return;
	}
	godot_libpd::PdCommand cmd;
	cmd.opcode = godot_libpd::PdCommand::MIDI;
	cmd.i32 = p_channel;
	cmd.i64 = ((int64_t)p_pitch << 32) | (int64_t)(p_velocity & 0x7F);
	worker.push_command(cmd);
}

int64_t LibpdInstance::instance_id() const {
	return worker.instance_id();
}

int LibpdInstance::samplerate() const {
	return samplerate_value;
}

bool LibpdInstance::patch_loaded() const {
	return has_patch.load();
}

bool LibpdInstance::dsp_active() const {
	return dsp_running.load();
}

uint64_t LibpdInstance::debug_blocks_pushed() const {
	return dry_sink.blocks_pushed();
}

float LibpdInstance::debug_sink_peak() const {
	return dry_sink.peak();
}

void LibpdInstance::_emit_failure(int p_code, const String &p_text) {
	emit_signal("failure", p_code, p_text);
}

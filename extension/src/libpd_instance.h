#pragma once

#include <atomic>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "libpd_worker.h"

namespace godot {

/**
 * One Pure Data instance (spec §4). Node-based: one instance per scene node.
 *
 * All libpd calls are executed on this instance's worker thread; main-thread
 * API pushes commands into the worker's queue. See spec §5 (threading model)
 * and §10 (error handling).
 */
class LibpdInstance : public Node {
	GDCLASS(LibpdInstance, Node)

public:
	LibpdInstance();
	virtual ~LibpdInstance();

	/// Initialize this pd instance. Returns false if already initialized or
	/// the worker failed to initialize pd.
	bool init(int p_samplerate = 44100, int p_n_ins = 0, int p_n_out = 2);

	/// Open a patch. p_path may be a res:// or absolute path.
	/// Returns Error::OK on success.
	int load_patch(const String &p_path, const PackedStringArray &p_search_paths = {});
	/// Close the current patch (stops dsp first if active).
	int unload_patch();

	/// Enable the dsp loop on the worker thread.
	int start_dsp();
	/// Disable the dsp loop.
	int stop_dsp();

	/// Send a message to a receiver in the patch (worker-thread execution).
	void send_pd_message(const String &p_receiver, const PackedStringArray &p_args);
	/// Convenience: send a float to a receiver (e.g. a sig~/(*~) path).
	void set_parameter(const String &p_path, double p_value);
	/// Send a MIDI note to the patch's notein objects.
	void send_midi(int p_channel, int p_pitch, int p_velocity);

	int64_t instance_id() const;
	int samplerate() const;
	bool patch_loaded() const;
	bool dsp_active() const;
	uint64_t debug_blocks_pushed() const;
	float debug_sink_peak() const;

public:
	void _enter_tree() override;
	void _exit_tree() override;

protected:
private:
	static void _bind_methods();

	/// Resolve a res:// or filesystem path to a pd-openable absolute path.
	/// Copies pack-only resources (res:// not on the filesystem) to the user
	/// data dir so the C library can open it.
	static String resolve_patch_path(const String &p_path, bool *r_ok);

	void _emit_failure(int p_code, const String &p_text);

	static int64_t _next_instance_id;

	int64_t instance_id_value = 0;
	godot_libpd::LibpdWorker worker;
	godot_libpd::DrySink dry_sink;
	std::atomic<bool> initialized{false};
	std::atomic<bool> has_patch{false};
	std::atomic<bool> dsp_running{false};
	int samplerate_value = 44100;
};

} // namespace godot

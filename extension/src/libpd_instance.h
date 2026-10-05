#pragma once

#include <atomic>

#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "core/pd_audio_sink_generator.h"
#include "libpd_worker.h"

#ifdef NATIVE_AUDIO
#include "core/mix_input_ring.h"
#endif

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

	/// Initialize this pd instance. Returns false if already initialized, the
	/// samplerate does not match the AudioServer mix rate, or the worker
	/// failed to initialize pd.
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

	/// Push a MIDI command onto the worker's command queue (thread-safe).
	/// Called from the LibpdServer's MIDI I/O thread via the router's
	/// on_midi_command callback, or from the main thread. The worker picks
	/// it up on its next command-poll and executes it on the worker thread
	/// (spec §5: input MIDI runs as worker-thread commands).
	void push_midi_command(const godot_libpd::PdCommand &p_command);

	/// The worker's bounded MIDI output queue. The server registers it with
	/// the router on _enter_tree (MIDI I/O thread drains it for MIDI
	/// output); the worker's pd output hooks feed it.
	godot_libpd::MidiOutputQueue *midi_output_queue() {
		return &worker.midi_out;
	}

	int64_t instance_id() const;
	int samplerate() const;
	bool patch_loaded() const;
	bool dsp_active() const;
	uint64_t debug_blocks_pushed() const;
	float debug_sink_peak() const;

	// --- Native audio role (M5; #ifdef NATIVE_AUDIO builds only) ---
	// Set BEFORE init(). SYNTH (default) renders into a MixInputRing gathered
	// by the mix-down callback; MIXER is the control-only mix-down instance
	// rendered by the PortAudio callback. Ignored on Android (no NATIVE_AUDIO),
	// where every instance uses the Godot generator path.
	enum Role { SYNTH, MIXER };
	void set_role(int p_role);
	/// Role as an int (0 = SYNTH, 1 = MIXER) — Godot cannot bind a raw C++
	/// enum return, so the binding surface is int; `LibpdInstance::Role::MIXER`
	/// (==1) still compares equal to the return value.
	int get_role() const;
	bool is_mixer() const;
	/// This worker's pd instance — valid after init() (happens-before via the
	/// INIT promise). The server uses it to bind the NativeAudio mix-down.
	struct _pdinstance *pd_instance_ptr() const;
	/// Channel shape recorded at init() (used by set_mixer validation).
	int init_n_ins() const;
	int init_n_out() const;

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

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
	// Instance role (M5): SYNTH (default) or MIXER.
	Role role = Role::SYNTH;
	// Channel shape recorded at init() (set_mixer validation, invariant #3).
	int init_n_ins_ = 0;
	int init_n_out_ = 2;
#ifdef NATIVE_AUDIO
	// SYNTH only: the ring the worker pushes rendered blocks into (owned by
	// this instance; created in init(), freed in _exit_tree after the worker
	// is joined). The server tracks it for NativeAudio gather/unregister.
	godot_libpd::MixInputRing *synth_ring = nullptr;
#endif
	godot_libpd::GeneratorSink sink;
	godot::Ref<godot::AudioStreamGenerator> generator;
	godot::AudioStreamPlayer *player = nullptr; // owned child
	std::atomic<bool> initialized{false};
	std::atomic<bool> has_patch{false};
	std::atomic<bool> dsp_running{false};
	int samplerate_value = 44100;
};

} // namespace godot

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/midi_output_queue.h"
#include "core/mix_input_ring.h"
#include "core/pd_audio_sink.h"
#include "core/pd_command_queue.h"
#include "core/pd_event_ring.h"

#include "m_pd.h" // t_pdinstance, libpd API
#include "z_libpd.h"

namespace godot_libpd {

/**
 * Per-instance worker: owns the pd instance and its command queue.
 *
 * Threading contract (spec §5): ALL libpd/pd calls happen on the worker
 * thread. The main thread only pushes commands and reads atomics.
 *
 * Lifecycle: start() spawns the thread; the thread runs until a
 * STOP_THREAD command, then frees the pd instance on its own thread and
 * exits; request_stop() + join() from the main thread.
 */
class LibpdWorker {
public:
	/**
	 * What the worker does with audio (M5 Task 5):
	 * - ANDROID: the existing a1/generator path — render, push to
	 *   config.sink (default: existing tests + Android unchanged).
	 * - SYNTH: render, but push each block to config.worker_ring — the
	 *   NativeAudio mix-down gathers it in the PortAudio callback.
	 * - MIXER: control-only — the PortAudio callback renders the mix
	 *   instance, so the worker never calls libpd_process_float; its
	 *   RT-sensitive control ops run under config.with_mixer_lock.
	 */
	enum class WorkerRole { SYNTH, MIXER, ANDROID };

	struct Config {
		int64_t instance_id = 0;
		int samplerate = 44100;
		int n_ins = 0;
		int n_out = 2;
		PdAudioSink *sink = nullptr;
		// Hook event delivery (thread-safe; may be null).
		std::function<void(const PdEvent &)> on_event;

		// M5 role (default: ANDROID — the existing a1/generator behavior).
		WorkerRole role = WorkerRole::ANDROID;
		// SYNTH only: ring the DSP loop pushes rendered blocks to instead
		// of config.sink. null = SYNTH renders to nowhere.
		MixInputRing *worker_ring = nullptr;
		// MIXER only: serialize RT-sensitive control ops with the
		// PortAudio callback's mix render (Task 4 finding). Unset = the
		// op runs as-is.
		std::function<void(std::function<void()>)> with_mixer_lock;
	};

	explicit LibpdWorker(Config p_config);
	~LibpdWorker();

	/** Spawn the worker thread (must happen before any command is pushed). */
	void start();

	/** Signal the worker to stop (safe even if the thread was never started). */
	void request_stop();

	/** Join the worker thread. Blocks until the thread has fully exited. */
	void join();

	bool is_running() const;

	/** Push a command (thread-safe). */
	void push_command(const PdCommand &p_command);

	/** Enable/disable the dsp loop (atomic; read by the worker each iteration). */
	void set_dsp(bool p_on) { dsp_on = p_on; }
	bool is_dsp_on() const { return dsp_on; }

	int64_t instance_id() const {
		return config.instance_id;
	}

	/**
	 * This worker's pd instance — valid after the INIT command completes,
	 * nullptr before. Task 6 uses this to bind the NativeAudio mix-down
	 * (set_mixer(mix.pd, mix_n_in, 2) on the mixer worker's instance).
	 */
	t_pdinstance *pd_instance_ptr() const {
		return pd_instance;
	}

	/// Called by the C printhook (worker thread context).
	void emit_print(const char *p_text);
	/// Called by the C noteonhook (worker thread context).
	void emit_note_on(int p_channel, int p_pitch, int p_velocity);

	/**
	 * Install the pd output hooks that feed midi_out (noteon, controlchange,
	 * programchange, pitchbend, aftertouch, polyaftertouch, midibyte) and
	 * set the instance data pointer used by the hook trampolines.
	 * Must run on the thread that owns the pd instance, once, before DSP
	 * can run (called from the INIT command path; libpd requires hook
	 * (un)setting while DSP is stopped).
	 */
	static void install_midi_output_hooks(void *p_worker_ptr);

	/**
	 * Bounded MIDI output queue (spec §4). Worker thread pushes from pd
	 * hooks; the MIDI I/O thread (Task 4) drains it for PortMIDI output.
	 */
	MidiOutputQueue midi_out;

private:
	void run();
	void execute_command(const PdCommand &p_command);

	/**
	 * Run p_op on the worker thread, serialized against the PortAudio
	 * callback ONLY for MIXER role with a configured with_mixer_lock
	 * (Task 4 finding: the callback renders the mix instance
	 * concurrently). SYNTH/ANDROID — and MIXER without a lock — run p_op
	 * as-is, exactly like the existing lock-free path.
	 */
	template <class F>
	void with_lock(F &&p_op) {
		if (config.role == WorkerRole::MIXER && config.with_mixer_lock) {
			config.with_mixer_lock(std::function<void()>(std::forward<F>(p_op)));
		} else {
			std::forward<F>(p_op)();
		}
	}

	/**
	 * Close the open patch (worker thread only). MIXER first re-binds
	 * this thread's pd_this to its own instance: libpd_closefile does
	 * NOT self-bind — it runs pd_free(canvas) under the calling
	 * thread's thread-local pd_this, and a prior libpd_free_instance on
	 * that thread would have reset the binding to the main libpd
	 * instance (Task 4 teardown finding). Harmless no-op if already
	 * bound.
	 */
	void close_patch();

	Config config;
	std::thread thread;
	PdCommandQueue queue;
	std::atomic<bool> stop_requested{false};
	std::atomic<bool> thread_started{false};
	std::atomic<bool> dsp_on{false};

	// Worker-thread-owned state (touched by the worker thread only).
	t_pdinstance *pd_instance = nullptr;
	void *patch_handle = nullptr;
	std::vector<float> out_buffer;
	int blocksize = 0;
	int n_out = 2;
	int samplerate = 44100;
	std::chrono::steady_clock::time_point next_tick{};
};

} // namespace godot_libpd

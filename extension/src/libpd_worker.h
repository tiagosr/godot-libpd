#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
	struct Config {
		int64_t instance_id = 0;
		int samplerate = 44100;
		int n_ins = 0;
		int n_out = 2;
		PdAudioSink *sink = nullptr;
		// Hook event delivery (thread-safe; may be null).
		std::function<void(const PdEvent &)> on_event;
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

	/// Called by the C printhook (worker thread context).
	void emit_print(const char *p_text);
	/// Called by the C noteonhook (worker thread context).
	void emit_note_on(int p_channel, int p_pitch, int p_velocity);

private:
	void run();
	void execute_command(const PdCommand &p_command);

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

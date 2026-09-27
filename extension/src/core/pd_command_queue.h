#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

namespace godot_libpd {

/**
 * One command the main thread pushes to an instance's worker thread.
 * Plain C data: no Godot types, no allocation. The worker executes commands
 * strictly on its own thread between dsp blocks (all libpd calls are
 * worker-thread-only).
 */
struct PdCommand {
	enum Opcode : uint32_t {
		INIT = 0,      // i32 = samplerate, i64 = n_ins*1000+n_out (packed)
		LOAD = 1,      // path = patch path, search = search path dir (may be NULL)
		UNLOAD = 2,
		MESSAGE = 3,   // path = receiver, args = space-joined args string
		MIDI = 4,      // i32 = channel, i64 = pitch*1000+velocity (packed)
		STOP_THREAD = 5,
	};

	uint32_t opcode = 0;
	int32_t i32 = 0;
	int64_t i64 = 0;
	const char *path = nullptr;  // caller-owned, valid until executed
	const char *args = nullptr;  // caller-owned, valid until executed
	const char *search = nullptr; // caller-owned, valid until executed
};

/**
 * Thread-safe FIFO. Multiple producers (in practice only the main thread),
 * one consumer (the worker). Pop supports a timeout so the consumer can
 * check its stop flag while waiting.
 */
class PdCommandQueue {
public:
	/** Push a command. Thread-safe, non-blocking. */
	void push(const PdCommand &p_command);

	/**
	 * Pop one command. Returns 1 if a command was popped, 0 on timeout.
	 * p_timeout_us: 0 = non-blocking; >0 = wait up to that many microseconds.
	 */
	int pop(PdCommand *r_command, uint32_t p_timeout_us);

	/** True if no command is currently queued. */
	bool empty() const;

private:
	mutable std::mutex mutex;
	std::condition_variable notify;
	std::deque<PdCommand> queue;
};

} // namespace godot_libpd

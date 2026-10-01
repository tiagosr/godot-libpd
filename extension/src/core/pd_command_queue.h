#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

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
		MIDI = 4,      // i32 = channel, i64 = pitch<<32 | velocity (packed)
		STOP_THREAD = 5,
		MIDI_NOTE = 6,            // i32 = channel(0-15), i64 = pitch*256+velocity
		MIDI_CC = 7,              // i32 = channel, i64 = controller*256+value
		MIDI_PROGRAM_CHANGE = 8,  // i32 = channel, i64 = program
		MIDI_PITCH_BEND = 9,      // i32 = channel, i64 = low*256+high (value = low+high*128)
		MIDI_AFTERTOUCH = 10,     // i32 = channel, i64 = pressure*256 (pressure = d1)
		MIDI_POLY_AFTERTOUCH = 11, // i32 = channel, i64 = pitch*256+pressure
		MIDI_BYTE = 12,           // i64 = byte (0-255)
		MIDI_SYSEX = 13,          // midi_len bytes in midi[0..midi_len-1], F0..F7 inclusive
	};

	uint32_t opcode = 0;
	int32_t i32 = 0;
	int64_t i64 = 0;
	std::string path;  // e.g. patch file, receiver, search dir
	std::string args;  // e.g. space-joined message args
	std::string search; // LOAD: search path dir (may be empty)
	// Optional: pointer to a std::shared_ptr<std::promise<int>> owned by the
	// main thread; the worker fulfils it with the call's result code.
	void *result = nullptr;
	// MIDI_SYSEX payload: up to 128 bytes, F0..F7 inclusive (zero-init keeps
	// the growth bounded; the queue is an unbounded std::deque).
	uint8_t midi[128] = {};
	uint32_t midi_len = 0;
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

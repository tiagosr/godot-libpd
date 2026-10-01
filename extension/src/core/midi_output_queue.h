#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace godot_libpd {

/**
 * One MIDI output event captured from a pd output hook on the worker
 * thread (spec §4). Plain C data. The MIDI I/O thread (Task 4) drains
 * the queue and writes it to PortMIDI — the worker never knows PortMIDI.
 */
struct MidiOutMsg {
	enum Kind : uint8_t {
		NOTE,             // channel, d1 = pitch, d2 = velocity
		CC,               // channel, d1 = controller, d2 = value
		PROGRAM_CHANGE,   // channel, d1 = program
		PITCH_BEND,       // channel, d1 = low 7 bits, d2 = high 7 bits (value = d1+d2*128)
		AFTERTOUCH,       // channel, d1 = pressure (d2 unused; matches MidiShortMsg)
		POLY_AFTERTOUCH,  // channel, d1 = pitch, d2 = pressure
		RAW_BYTE,         // byte ([midiout])
	};

	Kind kind = NOTE;
	uint8_t channel = 0; // NOTE/CC/PC/PB/AT/PAT: 0-15
	uint8_t d1 = 0;
	uint8_t d2 = 0;
	uint8_t byte = 0; // RAW_BYTE only
};

/**
 * Bounded, thread-safe FIFO for MIDI output events (spec §4). The worker
 * thread pushes from pd hooks; the MIDI I/O thread drains via pop_all().
 * Producers never wait: on overflow the OLDEST message is dropped, so a
 * blocked output can never stall the worker thread (spec §4 invariant:
 * audio never blocks on MIDI).
 */
class MidiOutputQueue {
public:
	static constexpr int CAPACITY = 4096;

	/** Push one message. Thread-safe, non-blocking; drops the oldest on overflow. */
	void push(const MidiOutMsg &p_msg) {
		std::lock_guard<std::mutex> lock(mutex);
		if ((int)queue.size() >= CAPACITY) {
			queue.pop_front();
			dropped_count++;
		}
		queue.push_back(p_msg);
	}

	/**
	 * Pop all pending messages in FIFO order. Returns the count popped
	 * (0 if empty). Single consumer (the MIDI I/O thread).
	 */
	int pop_all(std::vector<MidiOutMsg> &r_out) {
		std::lock_guard<std::mutex> lock(mutex);
		r_out.clear();
		r_out.reserve(queue.size());
		int n = 0;
		while (!queue.empty()) {
			r_out.push_back(queue.front());
			queue.pop_front();
			n++;
		}
		return n;
	}

	/** Number of messages dropped due to overflow since creation. */
	uint64_t dropped() const {
		std::lock_guard<std::mutex> lock(mutex);
		return dropped_count;
	}

private:
	mutable std::mutex mutex;
	std::deque<MidiOutMsg> queue;
	uint64_t dropped_count = 0;
};

} // namespace godot_libpd

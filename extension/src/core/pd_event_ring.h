#pragma once

#include <cstdint>
#include <mutex>

namespace godot_libpd {

/**
 * One event a worker thread (or a hook running on one) pushes toward the
 * main thread. Plain C struct: no Godot types, no allocation.
 */
struct PdEvent {
	enum Type : uint32_t {
		PRINT = 0,    // data[0..] = UTF-8 text (NUL-terminated, truncated to 63 bytes)
		NOTE_ON = 1,  // data[0]=channel, data[1]=pitch, data[2]=velocity
		DSP_ACTIVE = 2, // data[0] = bool
	};

	int64_t instance_id = 0;
	uint32_t type = PRINT;
	uint32_t _pad = 0;
	char data[64] = {};
};

/**
 * Bounded multi-producer / single-consumer ring, protected by one mutex.
 *
 * Design note: a mutex instead of a lock-free CAS ring — event rate is tiny
 * (blocks/sec per instance, a few prints/sec), the critical section is two
 * index updates and a struct copy, and a mutex is trivially correct at a
 * ring wrap where full/empty are otherwise indistinguishable. Producers
 * never wait for the consumer: when full, the oldest unread event is
 * dropped (print spam must never stall a worker thread).
 *
 * Capacity is fixed at 1024.
 */
class PdEventRing {
public:
	static size_t capacity() {
		return CAPACITY;
	}

	/** Push one event. Thread-safe, non-blocking. May drop the oldest event when full. */
	void push(const PdEvent &p_event);

	/**
	 * Pop up to p_max events. Returns the number popped (0 if empty).
	 * Must be called from a single consumer (the main thread).
	 */
	size_t pop(PdEvent *r_events, size_t p_max);

	/** Approximate unread count — for tests/diagnostics only. */
	size_t count_approx() const;

	/** Number of events dropped due to overflow since creation. */
	uint64_t dropped_count() const;

private:
	static constexpr size_t CAPACITY = 1024;

	mutable std::mutex mutex;
	PdEvent storage[CAPACITY];
	size_t head = 0; // next slot to read
	size_t tail = 0; // next slot to write
	size_t count = 0; // unread events (resolves full vs empty at wrap)
	uint64_t dropped = 0;
};

} // namespace godot_libpd

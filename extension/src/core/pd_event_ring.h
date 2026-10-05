#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>

namespace godot_libpd {

/**
 * One event a worker thread (or a hook running on one) pushes toward the
 * main thread. Plain C struct: no Godot types, no allocation.
 */

/**
 * Truncate p_src so it fits in p_max bytes on a UTF-8 boundary and
 * write it NUL-terminated into p_dst (capacity p_max + 1). A multi-
 * byte character straddling the boundary is dropped whole. Shared
 * by the print and receive-hook emit paths (spec: receive-hooks).
 */
inline void pd_event_truncate_utf8(const char *p_src, char *p_dst, int p_max) {
	size_t len = std::strlen(p_src);
	if (len > (size_t)p_max) {
		len = (size_t)p_max;
		while (len > 0 && ((static_cast<unsigned char>(p_src[len]) & 0xC0) == 0x80)) {
			len--;
		}
	}
	std::memcpy(p_dst, p_src, len);
	p_dst[len] = '\0';
}

struct PdEvent {
	static constexpr int k_max_list_items = 16;
	enum Type : uint32_t {
		PRINT = 0,    // data[0..] = UTF-8 text (NUL-terminated, truncated to 63 bytes)
		NOTE_ON = 1,  // data[0]=channel, data[1]=pitch, data[2]=velocity
		DSP_ACTIVE = 2, // data[0] = bool
		BANG = 3,     // data = `receive` name
		FLOAT = 4,    // data = `receive` name, fval = value
		SYMBOL = 5,   // data = `receive` name, sval = symbol text
		LIST = 6,     // data = `receive` name, list_* arrays (n_items items)
	};

	int64_t instance_id = 0;
	uint32_t type = PRINT;
	uint32_t _pad = 0;
	char data[64] = {};
	char sval[32] = {}; // SYMBOL only
	float fval = 0.0f;  // FLOAT only
	uint32_t n_items = 0; // LIST only (0..k_max_list_items)
	float list_floats[k_max_list_items] = {};
	char list_syms[k_max_list_items][32] = {};
	uint8_t list_is_symbol[k_max_list_items] = {};
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

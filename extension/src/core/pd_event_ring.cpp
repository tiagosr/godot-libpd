#include "pd_event_ring.h"

namespace godot_libpd {

void PdEventRing::push(const PdEvent &p_event) {
	std::lock_guard<std::mutex> lock(mutex);
	if (count == CAPACITY) {
		// Full: drop the oldest unread event, never wait for the consumer.
		head = head + 1;
		if (head == CAPACITY) {
			head = 0;
		}
		count--;
		dropped++;
	}
	storage[tail] = p_event;
	tail = tail + 1;
	if (tail == CAPACITY) {
		tail = 0;
	}
	count++;
}

size_t PdEventRing::pop(PdEvent *r_events, size_t p_max) {
	std::lock_guard<std::mutex> lock(mutex);
	size_t n = 0;
	while (n < p_max && count > 0) {
		r_events[n] = storage[head];
		head = head + 1;
		if (head == CAPACITY) {
			head = 0;
		}
		count--;
		n++;
	}
	return n;
}

size_t PdEventRing::count_approx() const {
	std::lock_guard<std::mutex> lock(mutex);
	return count;
}

uint64_t PdEventRing::dropped_count() const {
	std::lock_guard<std::mutex> lock(mutex);
	return dropped;
}

} // namespace godot_libpd

// Unit tests for PdEventRing and PdCommandQueue (Task 2).
// Plain assert-style harness: any failure prints and exits non-zero.

#include <atomic>
#include <cstdio>
#include <thread>

#include "core/pd_command_queue.h"
#include "core/pd_event_ring.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

static void test_ring_fifo_single_thread() {
	PdEventRing ring;
	for (int i = 0; i < 100; i++) {
		PdEvent e;
		e.instance_id = i;
		e.type = PdEvent::PRINT;
		e.data[0] = char(i & 0xFF);
		ring.push(e);
	}
	int got = 0;
	PdEvent out;
	while (ring.pop(&out, 1) == 1) {
		CHECK(out.instance_id == got);
		got++;
	}
	CHECK(got == 100);
	CHECK(ring.count_approx() == 0);
	std::printf("ring_fifo_single_thread done\n");
}

static void test_ring_concurrent_conservation() {
	PdEventRing ring;
	const int per_producer = 2000;

	auto producer = [&](int base) {
		for (int i = 0; i < per_producer; i++) {
			PdEvent e;
			e.instance_id = base;
			e.type = PdEvent::NOTE_ON;
			ring.push(e);
		}
	};

	std::thread t1(producer, 10);
	std::thread t2(producer, 20);
	t1.join();
	t2.join();

	// Drain everything.
	int popped = 0;
	PdEvent out[128];
	while (ring.count_approx() > 0) {
			popped += (int)ring.pop(out, 128);
		}

	// Conservation: every pushed event is either popped or accounted for as
	// dropped (never silently lost). With no consumer running, the ring
	// overflows deterministically — this pins the drop-oldest semantics.
	CHECK((uint64_t)popped + ring.dropped_count() == (uint64_t)(per_producer * 2));
	CHECK(popped == (int)ring.capacity());
	CHECK(ring.count_approx() == 0);
	std::printf("ring_concurrent_conservation done (popped=%d dropped=%llu)\n",
			popped, (unsigned long long)ring.dropped_count());
}

static void test_ring_overflow_drops_oldest() {
	PdEventRing ring;
	const int capacity = PdEventRing::capacity();
	for (int i = 0; i < capacity * 2; i++) {
		PdEvent e;
		e.instance_id = i;
		e.type = PdEvent::PRINT;
		ring.push(e);
	}
	int popped = 0;
	int first = -1;
	PdEvent out;
	while (ring.pop(&out, 1) == 1) {
		if (first == -1) {
			first = (int)out.instance_id;
		}
		popped++;
	}
	CHECK(popped == capacity);
	CHECK(first == capacity); // event #capacity (0-indexed) was the oldest kept
	std::printf("ring_overflow_drops_oldest done (capacity=%d)\n", capacity);
}

static void test_queue_fifo() {
	PdCommandQueue q;
	for (int i = 0; i < 100; i++) {
		PdCommand c;
		c.opcode = PdCommand::MESSAGE;
		c.i32 = i;
		q.push(c);
	}
	int got = 0;
	PdCommand c;
	while (q.pop(&c, 0) == 1) {
		CHECK(c.i32 == got);
		got++;
	}
	CHECK(got == 100);
	CHECK(q.empty());
	std::printf("queue_fifo done\n");
}

static void test_queue_concurrent_push() {
	PdCommandQueue q;
	const int total = 500;
	std::atomic<int> consumed{0};

	std::thread consumer([&] {
		PdCommand c;
		while (consumed < total) {
			if (q.pop(&c, 2000 /* us timeout */) == 1) {
				consumed++;
			}
		}
	});

	for (int i = 0; i < total; i++) {
		PdCommand c;
		c.opcode = PdCommand::MIDI;
		c.i32 = i;
		q.push(c);
	}
	consumer.join();
	CHECK(consumed == total);
	CHECK(q.empty());
	std::printf("queue_concurrent_push done\n");
}

static void test_queue_empty_after_drain() {
	PdCommandQueue q;
	CHECK(q.empty());
	PdCommand c;
	c.opcode = PdCommand::STOP_THREAD;
	q.push(c);
	CHECK(!q.empty());
	q.pop(&c, 0);
	CHECK(q.empty());
	std::printf("queue_empty_after_drain done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_ring_fifo_single_thread();
	test_ring_concurrent_conservation();
	test_ring_overflow_drops_oldest();
	test_queue_fifo();
	test_queue_concurrent_push();
	test_queue_empty_after_drain();

	if (failures == 0) {
		std::printf("ALL CORE TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

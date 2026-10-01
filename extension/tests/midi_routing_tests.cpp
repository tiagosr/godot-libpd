// Routing table tests (Task 3): pure, thread-safe fan-out routing logic.
// Plain assert-style harness: any failure prints and exits non-zero.

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/midi_routing_table.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

static void test_fanout_three_instances() {
	MidiRoutingTable table;
	// Fan one input port out to three instances, added out of order.
	table.add_route_in(1, 30);
	table.add_route_in(1, 10);
	table.add_route_in(1, 20);
	// Pinned: instances_for_port order is sorted.
	CHECK(table.instances_for_port(1) == (std::vector<int64_t>{10, 20, 30}));
	CHECK(table.instances_for_port(2).empty());

	// Out direction: one instance to three ports, added out of order.
	table.add_route_out(10, 3);
	table.add_route_out(10, 1);
	table.add_route_out(10, 2);
	CHECK(table.ports_for_instance(10) == (std::vector<int>{1, 2, 3}));
	CHECK(table.ports_for_instance(99).empty());
	std::printf("fanout_three_instances done\n");
}

static void test_idempotent_add() {
	MidiRoutingTable table;
	table.add_route_in(1, 10);
	table.add_route_in(1, 10); // double-add must not duplicate
	CHECK(table.instances_for_port(1) == (std::vector<int64_t>{10}));

	table.add_route_out(10, 1);
	table.add_route_out(10, 1); // double-add must not duplicate
	CHECK(table.ports_for_instance(10) == (std::vector<int>{1}));
	std::printf("idempotent_add done\n");
}

static void test_remove_absent() {
	MidiRoutingTable table;
	// Both directions: removing what was never added returns false.
	CHECK(table.remove_route_in(1, 10) == false);
	CHECK(table.remove_route_out(10, 1) == false);

	table.add_route_in(1, 10);
	CHECK(table.remove_route_in(1, 10) == true);
	CHECK(table.instances_for_port(1).empty());
	CHECK(table.remove_route_in(1, 10) == false); // already gone

	table.add_route_out(10, 1);
	CHECK(table.remove_route_out(10, 1) == true);
	CHECK(table.ports_for_instance(10).empty());
	CHECK(table.remove_route_out(10, 1) == false); // already gone
	std::printf("remove_absent done\n");
}

static void test_forget_instance() {
	MidiRoutingTable table;
	table.add_route_in(1, 10);
	table.add_route_in(1, 20);
	table.add_route_out(10, 5);
	table.add_route_out(20, 6);
	table.forget_instance(10);
	// Both directions of instance 10 cleared...
	CHECK(table.ports_for_instance(10).empty());
	CHECK(table.instances_for_port(1) == (std::vector<int64_t>{20}));
	// ...while the other instance stays intact in both directions.
	CHECK(table.instances_for_port(1) == (std::vector<int64_t>{20}));
	CHECK(table.ports_for_instance(20) == (std::vector<int>{6}));
	// Forgetting an unknown instance is a no-op.
	table.forget_instance(99);
	CHECK(table.instances_for_port(1) == (std::vector<int64_t>{20}));
	CHECK(table.ports_for_instance(20) == (std::vector<int>{6}));
	std::printf("forget_instance done\n");
}

static void test_concurrent_add_remove() {
	MidiRoutingTable table;
	// Pinned weaker check: no crash during concurrent add/remove, then
	// after join forget everything and verify both lookups are empty.
	// (Replay-consistency of the interleaved final state is NOT asserted.)
	const int ops = 10000;
	const auto worker = [&](int p_base) {
		for (int i = 0; i < ops; i++) {
			const int port = p_base + (i % 4);
			const int64_t instance = int64_t(p_base) * 100 + (i % 7);
			table.add_route_in(port, instance);
			table.add_route_out(instance, port);
			(void)table.remove_route_in(port, instance);
			(void)table.remove_route_out(instance, port);
			(void)table.instances_for_port(port);
			(void)table.ports_for_instance(instance);
		}
	};
	std::thread t1(worker, 100);
	std::thread t2(worker, 200);
	t1.join();
	t2.join();

	// Forget every instance either thread could have touched.
	for (int base = 100; base < 300; base += 100) {
		for (int i = 0; i < 7; i++) {
			table.forget_instance(int64_t(base) * 100 + i);
		}
	}
	// Every port/instance pair that was ever used must now be empty.
	for (int base = 100; base < 300; base += 100) {
		for (int i = 0; i < 4; i++) {
			CHECK(table.instances_for_port(base + i).empty());
		}
		for (int i = 0; i < 7; i++) {
			CHECK(table.ports_for_instance(int64_t(base) * 100 + i).empty());
		}
	}
	std::printf("concurrent_add_remove done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_fanout_three_instances();
	test_idempotent_add();
	test_remove_absent();
	test_forget_instance();
	test_concurrent_add_remove();

	if (failures == 0) {
		std::printf("ALL MIDI ROUTING TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

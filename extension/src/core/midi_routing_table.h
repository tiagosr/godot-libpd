#ifndef GD_LIBPD_MIDI_ROUTING_TABLE_H
#define GD_LIBPD_MIDI_ROUTING_TABLE_H

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace godot_libpd {

/**
 * Pure, thread-safe fan-out routing table for MIDI I/O (spec §7). Maps
 * MIDI input ports -> pd instances and pd instances -> MIDI output ports.
 * The MIDI I/O thread reads (instances_for_port / ports_for_instance)
 * while the main thread writes (add/remove/forget); every method is
 * guarded by a single std::mutex. Lookups return copies, so callers may
 * retain the result after further mutations. No Godot types, no libpd.
 */
class MidiRoutingTable {
public:
	/**
	 * Route MIDI input port p_port to pd instance p_instance.
	 * Idempotent: adding the same pair twice leaves one entry.
	 */
	void add_route_in(int p_port, int64_t p_instance) {
		std::lock_guard<std::mutex> lock(mutex);
		in_routes[p_port].insert(p_instance);
	}

	/**
	 * Remove the route from p_port to p_instance.
	 * Returns false if the route was absent.
	 */
	bool remove_route_in(int p_port, int64_t p_instance) {
		std::lock_guard<std::mutex> lock(mutex);
		auto it = in_routes.find(p_port);
		if (it == in_routes.end()) {
			return false;
		}
		if (it->second.erase(p_instance) == 0) {
			return false;
		}
		if (it->second.empty()) {
			in_routes.erase(it);
		}
		return true;
	}

	/** All instances routed to p_port, sorted ascending (empty if none). */
	std::vector<int64_t> instances_for_port(int p_port) const {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<int64_t> out;
		auto it = in_routes.find(p_port);
		if (it != in_routes.end()) {
			out.assign(it->second.begin(), it->second.end());
			std::sort(out.begin(), out.end());
		}
		return out;
	}

	/**
	 * Route pd instance p_instance to MIDI output port p_port.
	 * Idempotent: adding the same pair twice leaves one entry.
	 */
	void add_route_out(int64_t p_instance, int p_port) {
		std::lock_guard<std::mutex> lock(mutex);
		out_routes[p_instance].insert(p_port);
	}

	/**
	 * Remove the route from p_instance to p_port.
	 * Returns false if the route was absent.
	 */
	bool remove_route_out(int64_t p_instance, int p_port) {
		std::lock_guard<std::mutex> lock(mutex);
		auto it = out_routes.find(p_instance);
		if (it == out_routes.end()) {
			return false;
		}
		if (it->second.erase(p_port) == 0) {
			return false;
		}
		if (it->second.empty()) {
			out_routes.erase(it);
		}
		return true;
	}

	/** All MIDI output ports of p_instance, sorted ascending (empty if none). */
	std::vector<int> ports_for_instance(int64_t p_instance) const {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<int> out;
		auto it = out_routes.find(p_instance);
		if (it != out_routes.end()) {
			out.assign(it->second.begin(), it->second.end());
			std::sort(out.begin(), out.end());
		}
		return out;
	}

	/**
	 * Clear every route involving p_instance, in both directions (spec §7
	 * free-while-routed). Instances routed to the same ports, and ports
	 * of other instances, are left intact. Unknown instances are a no-op.
	 */
	void forget_instance(int64_t p_instance) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = in_routes.begin(); it != in_routes.end();) {
			if (it->second.erase(p_instance) != 0 && it->second.empty()) {
				it = in_routes.erase(it);
			} else {
				++it;
			}
		}
		out_routes.erase(p_instance);
	}

private:
	mutable std::mutex mutex;
	// MIDI input port -> pd instances receiving from it (fan-out).
	std::unordered_map<int, std::unordered_set<int64_t>> in_routes;
	// pd instance -> MIDI output ports it is sent to.
	std::unordered_map<int64_t, std::unordered_set<int>> out_routes;
};

} // namespace godot_libpd

#endif // GD_LIBPD_MIDI_ROUTING_TABLE_H

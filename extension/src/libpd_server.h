#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>

#include <unordered_set>

#include "core/pd_event_ring.h"

namespace godot {

/**
 * Project-level singleton (used via the `Libpd` autoload wrapper in the test
 * project; exactly one instance per project).
 *
 * Owns the cross-thread event ring: worker threads push PdEvents, the server
 * drains the ring in _process() (main thread only) and emits Godot signals.
 * No worker thread ever calls into Godot — this is the only thread-crossing
 * mechanism, which keeps the design deadlock-free (spec §4/§5).
 */
class LibpdServer : public Node {
	GDCLASS(LibpdServer, Node)

public:
	LibpdServer();
	virtual ~LibpdServer();

	/// Most recently created server (the project uses exactly one).
	static LibpdServer *get_singleton();

	/// True if the instance id was registered by a LibpdInstance node.
	bool instance_registered(int64_t p_instance_id) const;
	/// Number of registered instances.
	int instance_count() const;

	/// Test hook: push a PRINT event directly into the ring (used by the
	/// headless integration tests; real events arrive via the libpd hooks).
	void debug_push_print(int64_t p_instance_id, const String &p_text);

	void register_instance(int64_t p_instance_id);
	void unregister_instance(int64_t p_instance_id);

	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	void _drain_ring();

	godot_libpd::PdEventRing ring;
	std::unordered_set<int64_t> instances;
};

} // namespace godot

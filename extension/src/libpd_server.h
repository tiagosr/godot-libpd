#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/godot.hpp>

namespace godot {

/**
 * Singleton-style server node (project uses exactly one instance, as an
 * autoload). Task 1: empty registration only — the event ring, signal
 * fan-out and instance registry arrive in Tasks 3-6.
 */
class LibpdServer : public Node {
	GDCLASS(LibpdServer, Node)

public:
	LibpdServer();
	virtual ~LibpdServer();

private:
	static void _bind_methods();
};

} // namespace godot

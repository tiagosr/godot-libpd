#include "pd_command_queue.h"

#include <condition_variable>
#include <deque>

#include <chrono>

namespace godot_libpd {

void PdCommandQueue::push(const PdCommand &p_command) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		queue.push_back(p_command);
	}
	notify.notify_one();
}

int PdCommandQueue::pop(PdCommand *r_command, uint32_t p_timeout_us) {
	std::unique_lock<std::mutex> lock(mutex);
	if (p_timeout_us == 0) {
		if (queue.empty()) {
			return 0;
		}
	} else {
		notify.wait_for(lock, std::chrono::microseconds(p_timeout_us), [this] {
			return !queue.empty();
		});
		if (queue.empty()) {
			return 0;
		}
	}
	*r_command = queue.front();
	queue.pop_front();
	return 1;
}

bool PdCommandQueue::empty() const {
	std::lock_guard<std::mutex> lock(mutex);
	return queue.empty();
}

} // namespace godot_libpd

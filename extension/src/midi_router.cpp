// Task 4 — MidiRouter implementation: the MIDI I/O thread and the
// output path (MidiOutWriter-based raw-byte framing + full-form short
// messages, handed to the platform backend as byte triples). See
// midi_router.h for the pinned interface and thread invariants;
// task-4-report.md for the mapping from the brief's illustrative
// PortMIDI names to the vendored 2.0.7 API.
//
// v2 M2 Task 1: this file no longer touches Pm_* — all platform MIDI
// calls go through the MidiBackend interface (midi_backend.h),
// implemented by PortMidiBackend on macOS/Linux and (later) the
// Android RtMidi backend. The lock discipline carries over: no
// backend call is ever made while a router lock is held.

#include "midi_router.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace godot_libpd {

namespace {

constexpr int kMidiBufferEvents = 256; // open_* buffer size (brief: "queue, 256")
constexpr int kSignalRingCap = 1024; // input signal ring cap (drop-oldest)
constexpr int kControlTimeoutMs = 500; // open/close wait budget (spec §3/§7)
// App-created virtual port names (Task 7 on-device recipe: the A133 ALSA
// sequencer exposes no SUBS-capable ports, so the app must create its own;
// aconnect -l shows exactly these names on the backend client).
constexpr const char *kVirtualInName = "libpd test app in 0";
constexpr const char *kVirtualOutName = "libpd test app out 0";

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MidiRouter::MidiRouter() :
	MidiRouter(create_midi_backend()) {
}

MidiRouter::MidiRouter(std::unique_ptr<MidiBackend> p_backend) :
	backend_(std::move(p_backend)) {
	// A nullptr backend (stub / pre-RtMidi Android) or a failed
	// initialize() means no I/O thread: available() stays false and
	// open_* fail cleanly — the same inert-stub behavior the pre-v2
	// stub builds had. The backend initializes on the main thread
	// (Godot startup); its shutdown runs exactly once at the I/O
	// thread's exit (the io_loop exit path below).
	if (backend_ == nullptr || backend_->initialize() != MidiError::OK) {
		return;
	}
	// One-time identity log: the [MIDI] channel on stdout is the debug
	// channel for headless smokes and the A133 adb console (M3 Task 1
	// verification uses it to prove the smoke ran on RtMidi, not
	// PortMIDI).
	std::printf("[MIDI] backend=%s\n", backend_->backend_name());
	// Hotplug (M4): defer the first re-enumeration tick by one poll
	// interval so the consumer (the server, or a test) has time to bind
	// on_port_changed before the startup annotation fires. Set this before
	// the thread starts so the first io_loop tick does not use the header's
	// time_point::min() initializer (which would fire immediately).
	last_reeum_ = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		thread_running_ = true;
	}
	io_thread_ = std::thread(&MidiRouter::io_loop, this);
}

MidiRouter::~MidiRouter() {
	shutdown();
}

bool MidiRouter::available() const {
	// "Available" = backend initialized, NOT "at least one device":
	// the A133 enumerates zero devices (its ALSA ports carry no
	// SUBS_READ/SUBS_WRITE capability bits) yet open_virtual_*/
	// create_virtual_* still work there — the virtual ports are the
	// point.
	return backend_ != nullptr && backend_->available();
}

std::vector<std::pair<int, std::string>> MidiRouter::list_inputs() const {
	std::vector<std::pair<int, std::string>> out;
	if (backend_ != nullptr) {
		// No CountInputDevices on any platform backend: list all
		// devices and filter by the input side.
		const std::vector<MidiBackendPort> ports = backend_->list_ports();
		for (const MidiBackendPort &port : ports) {
			if (port.is_input) {
				out.emplace_back(port.index, port.name);
			}
		}
	}
	return out;
}

std::vector<std::pair<int, std::string>> MidiRouter::list_outputs() const {
	std::vector<std::pair<int, std::string>> out;
	if (backend_ != nullptr) {
		const std::vector<MidiBackendPort> ports = backend_->list_ports();
		for (const MidiBackendPort &port : ports) {
			if (port.is_output) {
				out.emplace_back(port.index, port.name);
			}
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Control queue (API thread -> I/O thread)
// ---------------------------------------------------------------------------

MidiRouter::ControlHandle MidiRouter::enqueue_control(ControlOpType p_op, int p_pm_index,
		int p_port_id, const std::string &p_name) {
	std::lock_guard<std::mutex> lock(control_mutex_);
	if (!thread_running_) {
		return ControlHandle{}; // no I/O thread (stub build, or init failed)
	}
	auto abandoned = std::make_shared<std::atomic<bool>>(false);
	auto done = std::make_shared<std::promise<int>>();
	ControlOp op;
	op.op = p_op;
	op.pm_index = p_pm_index;
	op.port_id = p_port_id;
	op.name = p_name;
	op.abandoned = abandoned;
	op.done = done;
	control_queue_.push_back(std::move(op));
	ControlHandle handle;
	handle.abandoned = abandoned;
	handle.result = done->get_future();
	return handle;
}

int MidiRouter::open_input(int p_pm_index) {
	ControlHandle handle = enqueue_control(ControlOpType::OPEN_INPUT, p_pm_index, -1);
	if (!handle.result.valid()) {
		return -1;
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		*handle.abandoned = true; // a late success self-closes the port
		notify_port_error(-1, "midi open timed out");
		return -1;
	}
	return handle.result.get();
}

int MidiRouter::open_output(int p_pm_index) {
	ControlHandle handle = enqueue_control(ControlOpType::OPEN_OUTPUT, p_pm_index, -1);
	if (!handle.result.valid()) {
		return -1;
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		*handle.abandoned = true; // a late success self-closes the port
		notify_port_error(-1, "midi open timed out");
		return -1;
	}
	return handle.result.get();
}

int MidiRouter::open_virtual_input() {
	ControlHandle handle =
			enqueue_control(ControlOpType::OPEN_VIRTUAL_INPUT, -1, -1);
	if (!handle.result.valid()) {
		return -1;
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		*handle.abandoned = true; // a late success self-closes the port
		notify_port_error(-1, "midi open timed out");
		return -1;
	}
	return handle.result.get();
}

int MidiRouter::open_virtual_output() {
	ControlHandle handle =
			enqueue_control(ControlOpType::OPEN_VIRTUAL_OUTPUT, -1, -1);
	if (!handle.result.valid()) {
		return -1;
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		*handle.abandoned = true; // a late success self-closes the port
		notify_port_error(-1, "midi open timed out");
		return -1;
	}
	return handle.result.get();
}

int MidiRouter::create_virtual_loopback(const std::string &p_name) {
	ControlHandle handle =
			enqueue_control(ControlOpType::CREATE_LOOPBACK, -1, -1, p_name);
	if (!handle.result.valid()) {
		return -1; // no I/O thread (stub build, or init failed)
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		// A late creation is harmless (the loopback owns no system
		// resource; it is listed until shutdown), but report the
		// timeout like the other control ops.
		notify_port_error(-1, "midi loopback create timed out");
		return -1;
	}
	return handle.result.get();
}

void MidiRouter::close_input(int p_port_id) {
	ControlHandle handle = enqueue_control(ControlOpType::CLOSE_INPUT, -1, p_port_id);
	if (!handle.result.valid()) {
		return; // no I/O thread: no ports can exist
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		// The op still completes on the I/O thread; report the timeout.
		notify_port_error(p_port_id, "midi close timed out");
	}
}

void MidiRouter::close_output(int p_port_id) {
	ControlHandle handle = enqueue_control(ControlOpType::CLOSE_OUTPUT, -1, p_port_id);
	if (!handle.result.valid()) {
		return;
	}
	if (handle.result.wait_for(std::chrono::milliseconds(kControlTimeoutMs)) !=
			std::future_status::ready) {
		notify_port_error(p_port_id, "midi close timed out");
	}
}

void MidiRouter::shutdown() {
	std::unique_lock<std::mutex> lock(control_mutex_);
	if (!thread_running_) {
		return; // idempotent; no thread (stub build, or init failed)
	}
	thread_running_ = false;
	ControlOp op;
	op.op = ControlOpType::SHUTDOWN;
	control_queue_.push_back(std::move(op));
	// Wait up to 500 ms for the I/O thread to close the ports and exit.
	// The wait must hold control_mutex_: the I/O thread sets thread_done_
	// under that mutex before notifying control_cv_ (unlocking first
	// makes wait_for throw std::system_error and abort the process).
	control_cv_.wait_for(lock, std::chrono::milliseconds(kControlTimeoutMs),
			[this] { return thread_done_; });
	if (!thread_done_) {
		// A wedged MIDI driver can hold the thread; surface it per spec
		// §3 and join anyway (v1 platforms deliver output immediately,
		// so this is a documented residual risk, not the expected path).
		std::fprintf(stderr,
				"godot-libpd: MIDI I/O thread did not exit within %d ms; "
				"joining anyway\n",
				kControlTimeoutMs);
		notify_port_error(-1, "midi i/o thread shutdown timed out");
	}
	if (io_thread_.joinable()) {
		io_thread_.join();
	}
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void MidiRouter::io_loop() {
	// A live backend is guaranteed here: the thread is only started
	// when backend_->initialize() succeeded (constructor).
	for (;;) {
		if (process_control_ops()) {
			break; // SHUTDOWN handled: exit path closes everything
		}
		// Snapshot the open input ports (port records are valid for
		// the whole loop: ports are only opened/closed on this thread).
		std::vector<std::pair<int, Port *>> input_ports;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &kv : ports_) {
				if (kv.second->in_use && kv.second->is_input &&
						kv.second->backend_handle != MidiBackend::NO_HANDLE) {
					input_ports.emplace_back(kv.first, kv.second.get());
				}
			}
		}
		// Stage 1 — read: backend poll into the per-port rings. The
		// per-port MidiReadStage converts each flagless 4-byte word
		// into raw stream bytes (sysex re-assembly); message framing
		// stays in the framer stage below.
		for (const auto &entry : input_ports) {
			const int pid = entry.first;
			Port *port = entry.second;
			const MidiBackend::PollResult result = backend_->poll_input(
					port->backend_handle, [&](uint32_t word) {
						port->read_stage.feed(word, [&](uint8_t byte) {
							port->ring.push(byte);
						});
					});
			if (result != MidiBackend::PollResult::OK) {
				handle_poll_error(pid, result);
				continue;
			}
			// Asynchronous host-error check (same loop, per stream).
			std::string text;
			if (backend_->has_host_error(port->backend_handle, text)) {
				notify_port_error(pid, text.c_str());
			}
		}
		// Stage 2 — frame + fan out (same thread, same iteration as the
		// read above; no parsing ever happens off this thread).
		for (const auto &entry : input_ports) {
			Port *port = entry.second;
			if (port->input == nullptr) {
				continue;
			}
			MidiFramer &framer = port->input->framer;
			port->ring.drain([&framer](uint8_t b) { framer.feed(b); });
		}
		// Stage 3 — output: drain instance queues into routed ports.
		output_stage();
		// Stage 4 — hotplug re-enumeration + diff (M4): time-gated by the
		// poll interval. The first tick is immediate (last_reeum_ starts at
		// time_point_min), which is what makes the startup baseline
		// annotated. All backend port ops + the diff run on this thread.
		{
			const auto now = std::chrono::steady_clock::now();
			const double interval = poll_interval_.load();
			if (now - last_reeum_ >=
					std::chrono::milliseconds((long)(interval * 1000.0))) {
				reenum_and_diff();
				last_reeum_ = now;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Exit: close every remaining backend stream on this thread, then
	// release the backend (close streams -> delete owned virtual
	// devices -> terminate, exactly once — the backend knows its own
	// ordering).
	std::vector<MidiBackend::PortHandle> handles;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : ports_) {
			Port &p = *kv.second;
			if (p.in_use && p.backend_handle != MidiBackend::NO_HANDLE) {
				handles.push_back(p.backend_handle);
				p.in_use = false;
				p.backend_handle = MidiBackend::NO_HANDLE;
			}
		}
	}
	for (const MidiBackend::PortHandle h : handles) {
		backend_->close(h); // best effort
	}
	backend_->shutdown();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : ports_) {
			Port &p = *kv.second;
			if (p.input != nullptr) {
				delete p.input;
				p.input = nullptr;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		thread_done_ = true;
	}
	control_cv_.notify_all();
}

bool MidiRouter::process_control_ops() {
	std::vector<ControlOp> ops;
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		while (!control_queue_.empty()) {
			ops.push_back(control_queue_.front());
			control_queue_.pop_front();
		}
	}
	for (auto &op : ops) {
		int result = -1;
		switch (op.op) {
			case ControlOpType::SHUTDOWN:
				if (op.done != nullptr) {
					op.done->set_value(0);
				}
				return true; // io_loop exits; its exit path closes the ports
			case ControlOpType::OPEN_INPUT:
			case ControlOpType::OPEN_OUTPUT:
				result = open_port(op.op == ControlOpType::OPEN_INPUT, op.pm_index);
				if (result >= 0 && op.abandoned != nullptr && op.abandoned->load()) {
					// The API side's 500 ms wait expired before this op
					// finished: close the port we just opened so no
					// orphaned stream leaks.
					close_port(result);
					result = -1;
				}
				break;
			case ControlOpType::OPEN_VIRTUAL_INPUT:
			case ControlOpType::OPEN_VIRTUAL_OUTPUT:
				result = open_virtual_port(op.op == ControlOpType::OPEN_VIRTUAL_INPUT);
				if (result >= 0 && op.abandoned != nullptr && op.abandoned->load()) {
					// Same late-success cleanup as OPEN_*; close_port
					// also deletes the owned virtual device (backend).
					close_port(result);
					result = -1;
				}
				break;
			case ControlOpType::CREATE_LOOPBACK: {
				// Hotplug (M4): the loopback ports are in-process (non-real).
				// Snapshot the index set before/after creation and mark any
				// newly-appearing indices non-real so the diff never auto-closes
				// them (they stay listed until shutdown).
				auto index_set = [this] {
					std::set<int> s;
					for (const auto &p : backend_->list_ports()) {
						s.insert(p.index);
					}
					return s;
				};
				const std::set<int> before = index_set();
				const bool ok =
						backend_->create_virtual_loopback(op.name) == MidiError::OK;
					if (ok) {
						for (int idx : index_set()) {
							if (!before.count(idx)) {
								non_real_indices_.insert(idx);
							}
						}
						result = 0;
					} else {
						notify_port_error(-1, backend_->last_error().c_str());
						result = -1;
					}
					break;
				}
				break;
			case ControlOpType::CLOSE_INPUT:
			case ControlOpType::CLOSE_OUTPUT:
				close_port(op.port_id);
				result = 0;
				break;
		}
		if (op.done != nullptr) {
			op.done->set_value(result);
		}
	}
	return false;
}

int MidiRouter::open_port(bool p_is_input, int p_device_index) {
	// The backend validates the index and opens under one lock scope
	// (A133 freeze repro — see PortMidiBackend::open_stream); the
	// router just maps the backend handle onto a new port record.
	MidiBackend::PortHandle handle = MidiBackend::NO_HANDLE;
	const MidiError err = p_is_input
			? backend_->open_input(p_device_index, kMidiBufferEvents, handle)
			: backend_->open_output(p_device_index, kMidiBufferEvents, handle);
	if (err == MidiError::OK) {
		int port_id = -1;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			port_id = next_port_id_++;
			auto port = std::make_unique<Port>();
			port->in_use = true;
			port->is_input = p_is_input;
			port->backend_handle = handle;
			// Hotplug (M4): capture the device name (for the (direction,name)
			// diff match) and the real_device flag (false for indices the
			// router knows are virtual/loopback, so they are never auto-closed).
			port->real_device = non_real_indices_.count(p_device_index) == 0;
			for (const auto &d : backend_->list_ports()) {
				if (d.index == p_device_index &&
						(p_is_input ? d.is_input : d.is_output)) {
					port->device_name = d.name;
					break;
				}
			}
			if (p_is_input) {
				port->input = new PortInput(this, port_id);
			}
			ports_[port_id] = std::move(port);
		}
		return port_id;
	}
	// The backend composed the full error text (including the
	// " (pm device N)" suffix on PortMidi); the user callback fires
	// without any lock held.
	notify_port_error(-1, backend_->last_error().c_str());
	return -1;
}

int MidiRouter::open_virtual_port(bool p_is_input) {
	// Create the virtual device first, then open the returned device id
	// through the same open path as index-based opens. The device id is
	// only known after creation, which is why this runs on the I/O
	// thread as its own control op. On success the backend tracks the
	// device's ownership: close_port / the io_loop exit delete it
	// after the stream closes; a failed open deletes it (no leak).
	int device_index = -1;
	const MidiError err = p_is_input
			? backend_->create_virtual_input(kVirtualInName, device_index)
			: backend_->create_virtual_output(kVirtualOutName, device_index);
	if (err != MidiError::OK) {
		notify_port_error(-1, backend_->last_error().c_str());
		return -1;
	}
	// Hotplug (M4): the created virtual device is app-owned, not an OS
	// endpoint — mark its index non-real so the diff never auto-closes it.
	non_real_indices_.insert(device_index);
	return open_port(p_is_input, device_index);
}

void MidiRouter::close_port(int p_port_id) {
	MidiBackend::PortHandle handle = MidiBackend::NO_HANDLE;
	bool is_input = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = ports_.find(p_port_id);
		if (it == ports_.end() || !it->second->in_use) {
			return; // unknown port, or already closed
		}
		handle = it->second->backend_handle;
		is_input = it->second->is_input;
	}
	// Close the backend stream (this thread owns it); if the port
	// created a virtual device, the backend deletes it after the
	// stream closes. Pm_Close alone does not remove it (ALSA's
	// alsa_in_close keeps virtual ports open on purpose — the port IS
	// the device), and Pm_DeleteVirtualDevice refuses an open device.
	// No router lock is held for the backend call (lock discipline,
	// midi_router.h).
	if (handle != MidiBackend::NO_HANDLE) {
		backend_->close(handle);
	}
	// Auto-unroute the port (spec §7: close stops the port, unroutes it).
	if (is_input) {
		const std::vector<int64_t> instances =
				in_routes_.instances_for_port(p_port_id);
		for (int64_t inst : instances) {
			in_routes_.remove_route_in(p_port_id, inst);
		}
	} else {
		std::vector<int64_t> instances;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = out_port_instances_.find(p_port_id);
			if (it != out_port_instances_.end()) {
				instances.assign(it->second.begin(), it->second.end());
			}
		}
		for (int64_t inst : instances) {
			out_routes_.remove_route_out(inst, p_port_id);
		}
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = ports_.find(p_port_id);
		if (it != ports_.end()) {
			Port &p = *it->second;
			p.in_use = false;
			p.backend_handle = MidiBackend::NO_HANDLE;
			p.ring.reset();
			p.read_stage.reset();
			if (p.input != nullptr) {
				delete p.input;
				p.input = nullptr;
			}
		}
		out_port_instances_.erase(p_port_id);
	}
	// Drop this port's raw-byte framing state (I/O-thread-only map).
	for (auto &kv : raw_writers_) {
		kv.second.erase(p_port_id);
	}
}

void MidiRouter::set_poll_interval(double p_seconds) {
	poll_interval_.store(p_seconds);
}

int MidiRouter::reenum_and_diff() {
	const std::vector<MidiBackendPort> ports = backend_->list_ports();
	// Build the current (direction, name) -> index map. A device that has
	// both sides yields two keys (one per direction).
	std::map<PortKey, int> current;
	for (const auto &p : ports) {
		if (p.is_input) {
			current[PortKey{true, p.name}] = p.index;
		}
		if (p.is_output) {
			current[PortKey{false, p.name}] = p.index;
		}
	}
	// Transient-empty debounce: an empty enumeration is only trusted after
	// two consecutive empty ticks — a bad probe tick must not close live
	// ports, yet a genuine "only device unplugged" must still be caught.
	if (current.empty() && !seen_.empty() && !pending_empty_) {
		pending_empty_ = true;
		return 0;
	}
	pending_empty_ = false;
	int changes = 0;
	// Adds: in current, not in seen_. Fire with the current index.
	for (const auto &kv : current) {
		if (seen_.count(kv.first) == 0) {
			const char *kind = kv.first.is_input ? "input" : "output";
			if (on_port_changed) {
				on_port_changed(true, kind, kv.second, kv.first.name.c_str());
			}
			++changes;
		}
	}
	// Removals: in seen_, not in current (if current is empty, this is all).
	for (const PortKey &key : seen_) {
		if (current.count(key) == 0) {
			const int remembered = last_index_.count(key) ? last_index_[key] : -1;
			const char *kind = key.is_input ? "input" : "output";
			if (on_port_changed) {
				on_port_changed(false, kind, remembered, key.name.c_str());
			}
			++changes;
			close_real_device_ports(key.is_input, key.name);
		}
	}
	// Update state (I/O thread only).
	seen_.clear();
	last_index_.clear();
	for (const auto &kv : current) {
		seen_.insert(kv.first);
		last_index_[kv.first] = kv.second;
	}
	return changes;
}

void MidiRouter::close_real_device_ports(bool p_is_input, const std::string &p_name) {
	std::vector<int> pids;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : ports_) {
			Port &p = *kv.second;
			if (p.in_use && p.is_input == p_is_input && p.real_device &&
					p.device_name == p_name) {
				pids.push_back(kv.first);
			}
		}
	}
	for (int pid : pids) {
		close_port(pid);
		notify_port_error(pid, "device removed");
	}
}

void MidiRouter::handle_poll_error(int p_port_id, MidiBackend::PollResult p_result) {
	if (p_result == MidiBackend::PollResult::BUFFER_OVERFLOW) {
		// The backend flushed its input buffer on overflow. Drop the
		// partial ring and reset the framer so no torn message is
		// decoded; keep the port open (per Pm_Read's docs, ordinary
		// processing resumes as soon as a new message arrives).
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = ports_.find(p_port_id);
			if (it != ports_.end()) {
				it->second->ring.reset();
				it->second->read_stage.reset();
				if (it->second->input != nullptr) {
					it->second->input->framer.reset();
				}
			}
		}
		notify_port_error(p_port_id, backend_->last_error().c_str());
		return;
	}
	// FATAL (pmDeviceRemoved / pmHostError / anything else): the port
	// is dead — drop the handle, unroute, and notify. The backend
	// composed the error text during the poll.
	const std::string text = backend_->last_error();
	close_port(p_port_id);
	notify_port_error(p_port_id, text.c_str());
}

MidiRouter::Port *MidiRouter::port_ptr(int p_port_id) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = ports_.find(p_port_id);
	return it != ports_.end() ? it->second.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Output stage (I/O thread)
// ---------------------------------------------------------------------------

void MidiRouter::output_stage() {
	std::vector<int64_t> instance_ids;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		instance_ids.reserve(instances_.size());
		for (auto &kv : instances_) {
			instance_ids.push_back(kv.first);
		}
		// Prune raw-byte framing state for instances no longer registered
		// (forget_instance runs on the API thread and cannot touch the
		// I/O-thread-only map).
		for (auto it = raw_writers_.begin(); it != raw_writers_.end();) {
			bool live = false;
			for (int64_t id : instance_ids) {
				if (id == it->first) {
					live = true;
					break;
				}
			}
			it = live ? std::next(it) : raw_writers_.erase(it);
		}
	}
	for (int64_t instance : instance_ids) {
		std::vector<MidiOutMsg> msgs;
		std::vector<int> port_ids;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = instances_.find(instance);
			if (it == instances_.end() || it->second.queue == nullptr) {
				continue;
			}
			// pop_all under mutex_ so forget_instance (which erases the
			// map entry) can never race the queue access; the queue
			// object itself outlives forget (Task 5 teardown order).
			if (it->second.queue->pop_all(msgs) <= 0) {
				continue;
			}
			// Nested lock order is always router -> table (never
			// inverted anywhere else in the router).
			port_ids = out_routes_.ports_for_instance(instance);
		}
		for (int port_id : port_ids) {
			MidiBackend::PortHandle handle = MidiBackend::NO_HANDLE;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto pit = ports_.find(port_id);
				if (pit != ports_.end() && pit->second->in_use &&
						pit->second->backend_handle != MidiBackend::NO_HANDLE) {
					handle = pit->second->backend_handle;
				}
			}
			if (handle == MidiBackend::NO_HANDLE) {
				continue; // port closed in the meantime: drop the messages
			}
			// No router lock during delivery: ports_ and raw_writers_
			// are I/O-thread-owned; backend calls are never made under
			// a router lock (see the lock discipline in midi_router.h).
			auto &writers = raw_writers_[instance];
			for (const MidiOutMsg &msg : msgs) {
				if (!deliver_out_msg(handle, port_id, writers, msg)) {
					// Write failed: write_short / write_sysex_msg
					// already closed and unrouted the port (its stream
					// and raw-byte framing state are gone) — drop the
					// rest of this port's batch; writing to the closed
					// stream would be UB.
					break;
				}
			}
		}
	}
}

bool MidiRouter::deliver_out_msg(MidiBackend::PortHandle p_handle, int p_port_id,
		std::unordered_map<int, RawRouteState> &p_writers,
		const MidiOutMsg &p_msg) {
	if (p_msg.kind == MidiOutMsg::RAW_BYTE) {
		return raw_byte_to_stream(p_handle, p_port_id, p_writers, p_msg.byte);
	}
	if (p_msg.kind == MidiOutMsg::SYSEX) {
		return write_sysex_msg(p_handle, p_port_id, p_msg.sysex);
	}
	// Channels are masked to the low nibble: output hooks may carry
	// pd_channel + 16*pd_port; the port nibble is dropped (v1
	// constraint, spec §6).
	const uint8_t channel = p_msg.channel & 0x0F;
	uint8_t status = 0;
	uint8_t d1 = 0;
	uint8_t d2 = 0;
	switch (p_msg.kind) {
		case MidiOutMsg::NOTE:
			status = (p_msg.d2 == 0 ? 0x80 : 0x90) | channel; // vel 0 -> note off
			d1 = p_msg.d1;
			d2 = p_msg.d2;
			break;
		case MidiOutMsg::CC:
			status = 0xB0 | channel;
			d1 = p_msg.d1;
			d2 = p_msg.d2;
			break;
		case MidiOutMsg::PROGRAM_CHANGE:
			status = 0xC0 | channel;
			d1 = p_msg.d1;
			break;
		case MidiOutMsg::AFTERTOUCH:
			status = 0xD0 | channel;
			d1 = p_msg.d1;
			break;
		case MidiOutMsg::PITCH_BEND:
			status = 0xE0 | channel;
			d1 = p_msg.d1;
			d2 = p_msg.d2;
			break;
		case MidiOutMsg::POLY_AFTERTOUCH:
			status = 0xA0 | channel;
			d1 = p_msg.d1;
			d2 = p_msg.d2;
			break;
		case MidiOutMsg::RAW_BYTE:
			return true; // handled above
		case MidiOutMsg::SYSEX:
			return true; // handled above
	}
	return write_short(p_handle, p_port_id, status, d1, d2);
}

bool MidiRouter::raw_byte_to_stream(MidiBackend::PortHandle p_handle, int p_port_id,
		std::unordered_map<int, RawRouteState> &p_writers,
		uint8_t p_byte) {
	RawRouteState &st = p_writers[p_port_id]; // default-constructed per route
	// The pinned MidiOutWriter transform (its per-call return is the wire
	// bytes for this input byte). The framing below mirrors that state
	// machine in lockstep — same transitions — and additionally tracks
	// message completion, because MidiBackend::write needs full-form
	// {status, d1, d2} bytes (running-status input is re-expanded here).
	const std::vector<uint8_t> emitted = st.writer.feed(p_byte);
	(void)emitted; // wire bytes == input bytes except where suppressed
	const uint8_t b = p_byte;
	if (st.in_sysex) {
		if (b == 0xF7) {
			st.in_sysex = false; // EOX: dropped, no status change
			return true;
		}
		if (b >= 0x80 && b != 0xF0) {
			st.in_sysex = false; // truncated sysex: fall through, status is real
		} else {
			return true; // sysex data (incl. nested F0): dropped whole
		}
	}
	if (b >= 0x80) {
		if (b == 0xF0) {
			st.in_sysex = true; // raw F0..F7 dropped (no sysex output path
			// on the raw-byte route; full sysex goes out via SYSEX)
			return true;
		}
		if (b >= 0xF8) {
			// Realtime: complete one-byte message, always sent (never
			// subject to running status).
			return write_short(p_handle, p_port_id, b, 0, 0);
		}
		// Status byte (emitted, or running-suppressed — the wire status
		// is st.status, which equals b in the suppressed case) starts a
		// new message; any incomplete previous message is dropped.
		st.status = b;
		st.have_status = true;
		st.data_need = MidiReadStage::raw_data_need(b);
		st.data_have = 0;
		return true;
	}
	// Data byte.
	if (!st.have_status || st.in_sysex) {
		return true; // stray data before any status: dropped by framing
	}
	if (st.data_have >= st.data_need) {
		return true; // previous message already complete: drop the extra byte
	}
	st.data[st.data_have++] = b;
	if (st.data_have < st.data_need) {
		return true;
	}
	// Message complete: send the full-form message.
	const uint8_t d2 = (st.data_need == 2) ? st.data[1] : 0;
	return write_short(p_handle, p_port_id, st.status, st.data[0], d2);
}

bool MidiRouter::write_short(MidiBackend::PortHandle p_handle, int p_port_id,
		uint8_t p_status, uint8_t p_d1, uint8_t p_d2) {
	const uint8_t bytes[3] = {p_status, p_d1, p_d2};
	const MidiError err = backend_->write(p_handle, bytes, 3);
	if (err == MidiError::OK) {
		return true;
	}
	// Spec §7 (approved mapping: any backend write failure, notably
	// device-removed / host errors on PortMidi): drop the handle,
	// unroute the port, and notify. Backend calls are never made under
	// a router lock; close_port takes the locks it needs on its own.
	//
	// The port is dead now: returning false makes output_stage drop
	// the rest of the batch (writing to the closed stream would be
	// UB). Error text came from the backend; the user callback fires
	// without any lock held.
	const std::string text = backend_->last_error();
	close_port(p_port_id);
	notify_port_error(p_port_id, text.c_str());
	return false;
}

bool MidiRouter::write_sysex_msg(MidiBackend::PortHandle p_handle, int p_port_id,
		const std::vector<uint8_t> &p_sysex) {
	const MidiError err = backend_->write_sysex(p_handle,
			p_sysex.data(), static_cast<int>(p_sysex.size()));
	if (err == MidiError::OK) {
		return true;
	}
	const std::string text = backend_->last_error();
	if (err == MidiError::InvalidParameter) {
		// A malformed payload is a logic error, not a port fault:
		// report it and drop the message, but keep the port open.
		notify_port_error(p_port_id, text.c_str());
		return false;
	}
	// Stream failure: same drop-the-port semantics as write_short.
	close_port(p_port_id);
	notify_port_error(p_port_id, text.c_str());
	return false;
}

// ---------------------------------------------------------------------------
// Input fan-out (I/O thread; called from the per-port framer sink)
// ---------------------------------------------------------------------------

void MidiRouter::on_input_short(int p_port_id, const MidiShortMsg &p_msg) {
	push_signal_event(MidiSignalEvent{false, p_port_id, p_msg, {}});
	const std::vector<int64_t> instances = in_routes_.instances_for_port(p_port_id);
	if (instances.empty()) {
		return;
	}
	PdCommand cmd;
	if (!build_command(p_msg, cmd)) {
		return; // defensive: on_short only carries high-level kinds
	}
	for (int64_t inst : instances) {
		deliver_command(inst, cmd);
	}
}

void MidiRouter::on_input_byte(int p_port_id, uint8_t p_byte) {
	// One MIDI_BYTE command per raw stream byte per routed instance
	// (dual delivery, spec §5). Realtime/system-common bytes arrive here
	// only; bytes inside a sysex never do (framer contract).
	const std::vector<int64_t> instances = in_routes_.instances_for_port(p_port_id);
	if (instances.empty()) {
		return;
	}
	PdCommand cmd{};
	cmd.opcode = PdCommand::MIDI_BYTE;
	cmd.i64 = p_byte;
	for (int64_t inst : instances) {
		deliver_command(inst, cmd);
	}
}

void MidiRouter::on_input_sysex(int p_port_id, const uint8_t *p_data, int p_len) {
	// p_data: the sysex body (F0-exclusive, F7-inclusive), <= 127 bytes.
	MidiSignalEvent ev;
	ev.is_sysex = true;
	ev.port_id = p_port_id;
	ev.sysex.assign(p_data, p_data + p_len);
	push_signal_event(ev);
	const std::vector<int64_t> instances = in_routes_.instances_for_port(p_port_id);
	if (instances.empty()) {
		return;
	}
	// One MIDI_SYSEX command per routed instance: the full F0..F7
	// sequence (<= 128 bytes, exactly the PdCommand::midi buffer). Its
	// bytes are NOT delivered as MIDI_BYTE commands.
	PdCommand cmd{};
	cmd.opcode = PdCommand::MIDI_SYSEX;
	cmd.midi[0] = 0xF0;
	for (int i = 0; i < p_len; ++i) {
		cmd.midi[1 + i] = p_data[i];
	}
	cmd.midi_len = static_cast<uint32_t>(1 + p_len);
	for (int64_t inst : instances) {
		deliver_command(inst, cmd);
	}
}

void MidiRouter::on_input_sysex_truncated(int p_port_id) {
	// Warning-grade port event: the first 127 bytes were already
	// delivered (signal + MIDI_SYSEX command); the tail is dropped by
	// the framer (its SYSEX_MAX cap).
	notify_port_error(p_port_id, "sysex truncated (>127 bytes)");
}

void MidiRouter::push_signal_event(const MidiSignalEvent &p_event) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (signal_events_.size() >= static_cast<size_t>(kSignalRingCap)) {
		signal_events_.erase(signal_events_.begin()); // drop the oldest
	}
	signal_events_.push_back(p_event);
}

void MidiRouter::deliver_command(int64_t p_instance, const PdCommand &p_cmd) {
	if (on_midi_command) {
		on_midi_command(p_instance, p_cmd);
	}
}

bool MidiRouter::build_command(const MidiShortMsg &p_msg, PdCommand &r_cmd) {
	r_cmd = PdCommand{};
	r_cmd.i32 = p_msg.channel; // channels 0-15, passed to libpd unchanged
	switch (p_msg.kind) {
		case MidiKind::NOTE_ON:
		case MidiKind::NOTE_OFF:
			r_cmd.opcode = PdCommand::MIDI_NOTE;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::CC:
			r_cmd.opcode = PdCommand::MIDI_CC;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::PROGRAM_CHANGE:
			r_cmd.opcode = PdCommand::MIDI_PROGRAM_CHANGE;
			r_cmd.i64 = p_msg.d1;
			break;
		case MidiKind::PITCH_BEND:
			r_cmd.opcode = PdCommand::MIDI_PITCH_BEND;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		case MidiKind::AFTERTOUCH:
			r_cmd.opcode = PdCommand::MIDI_AFTERTOUCH;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256;
			break;
		case MidiKind::POLY_AFTERTOUCH:
			r_cmd.opcode = PdCommand::MIDI_POLY_AFTERTOUCH;
			r_cmd.i64 = static_cast<int64_t>(p_msg.d1) * 256 + p_msg.d2;
			break;
		default:
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Routing / instance registration / signal drain (API thread)
// ---------------------------------------------------------------------------

void MidiRouter::route_input(int p_port_id, int64_t p_instance, bool p_add) {
	if (p_add) {
		in_routes_.add_route_in(p_port_id, p_instance);
	} else {
		in_routes_.remove_route_in(p_port_id, p_instance);
	}
}

void MidiRouter::route_output(int64_t p_instance, int p_port_id, bool p_add) {
	if (p_add) {
		out_routes_.add_route_out(p_instance, p_port_id);
		std::lock_guard<std::mutex> lock(mutex_);
		out_port_instances_[p_port_id].insert(p_instance);
	} else {
		out_routes_.remove_route_out(p_instance, p_port_id);
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = out_port_instances_.find(p_port_id);
		if (it != out_port_instances_.end()) {
			it->second.erase(p_instance);
			if (it->second.empty()) {
				out_port_instances_.erase(it);
			}
		}
	}
}

void MidiRouter::register_instance_output(int64_t p_instance, MidiOutputQueue *p_queue) {
	std::lock_guard<std::mutex> lock(mutex_);
	instances_[p_instance].queue = p_queue;
}

void MidiRouter::forget_instance(int64_t p_instance) {
	const std::vector<int> out_ports = out_routes_.ports_for_instance(p_instance);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		instances_.erase(p_instance);
		for (int port_id : out_ports) {
			auto it = out_port_instances_.find(port_id);
			if (it != out_port_instances_.end()) {
				it->second.erase(p_instance);
				if (it->second.empty()) {
					out_port_instances_.erase(it);
				}
			}
		}
	}
	in_routes_.forget_instance(p_instance);
	out_routes_.forget_instance(p_instance);
	// raw_writers_ entries are pruned by the I/O thread's next
	// output_stage pass (I/O-thread-only map).
}

void MidiRouter::drain_signal_events(std::vector<MidiSignalEvent> &r_out) {
	std::lock_guard<std::mutex> lock(mutex_);
	r_out.clear();
	r_out.reserve(signal_events_.size());
	for (auto &ev : signal_events_) {
		r_out.push_back(std::move(ev));
	}
	signal_events_.clear();
}

// ---------------------------------------------------------------------------
// Error notification
// ---------------------------------------------------------------------------

void MidiRouter::notify_port_error(int p_port_id, const char *p_what) {
	if (on_port_error) {
		on_port_error(p_port_id, p_what);
	}
}

} // namespace godot_libpd

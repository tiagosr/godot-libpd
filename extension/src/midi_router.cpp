// Task 4 — MidiRouter implementation: PortMIDI integration, the MIDI I/O
// thread, and the output path (MidiOutWriter-based raw-byte framing +
// full-form Pm_WriteShort words). See midi_router.h for the pinned
// interface and thread invariants; task-4-report.md for the mapping from
// the brief's illustrative PortMIDI names to the vendored 2.0.7 API.

#include "midi_router.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace godot_libpd {

namespace {

constexpr int32_t kPmBufferEvents = 256; // Pm_Open* bufferSize (brief: "queue, 256")
constexpr int32_t kPmReadBatch = 32; // Pm_Read events per poll (~1 ms loop)
constexpr int kSignalRingCap = 1024; // input signal ring cap (drop-oldest)
constexpr int kControlTimeoutMs = 500; // open/close wait budget (spec §3/§7)

#ifdef PORTMIDI_ENABLED
// PmEvent.message layout (portmidi.h / PmMakeEvent): 1-4 bytes of MIDI
// data, low byte first; the high bit (0xFF000000) marks a long (sysex)
// event — this is the 2.0.7 replacement for the brief's
// PmReadLong/PmIsLongMessage pair.
constexpr uint32_t kPmLongFlag = 0xFF000000;

// Total bytes a short (non-sysex) PmEvent carries: pm_midi_length()
// semantics from portmidi.c.
int pm_short_bytes(uint32_t p_message) {
	const uint8_t status = static_cast<uint8_t>(p_message & 0xFF);
	if (status < 0x80) {
		return 1; // not expected for short events; safe default
	}
	const uint8_t type = status & 0xF0;
	if (type == 0xC0 || type == 0xD0) {
		return 2; // program change, channel aftertouch
	}
	if (type == 0xF0) {
		return status == 0xF2 ? 3 : 2; // song position; meta/sonoselect/tune/EOX
	}
	if (type == 0xF8) {
		return 1; // realtime F8..FF
	}
	return 3; // 0x80..0xBF (channel), 0xE0 (pitch bend)
}

// Data bytes expected after a status byte in a raw [midiout] stream
// (total message bytes - 1).
int raw_data_need(uint8_t p_status) {
	if (p_status >= 0xF8) {
		return 0; // realtime: complete one-byte messages
	}
	const uint8_t type = p_status & 0xF0;
	if (type == 0xC0 || type == 0xD0) {
		return 1;
	}
	if (type == 0xF0) {
		return p_status == 0xF2 ? 2 : 1;
	}
	return 2;
}
#endif // PORTMIDI_ENABLED

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MidiRouter::MidiRouter() {
#ifdef PORTMIDI_ENABLED
	// Pm_Initialize must run on one consistent thread, and this router is
	// the process's only PortMIDI user, so initialization happens here
	// (main thread, Godot startup) and Pm_Terminate runs exactly once at
	// the I/O thread's exit (shutdown path) — the object's lifetime
	// bounds all PM use (macOS same-thread requirement). If
	// Pm_Initialize fails: no I/O thread; available() stays false and
	// open_* fail cleanly.
	pm_initialized_ = with_pm_mutex([] { return Pm_Initialize(); }) == pmNoError;
	if (pm_initialized_) {
		{
			std::lock_guard<std::mutex> lock(control_mutex_);
			thread_running_ = true;
		}
		io_thread_ = std::thread(&MidiRouter::io_loop, this);
	}
#endif
}

MidiRouter::~MidiRouter() {
	shutdown();
}

bool MidiRouter::available() const {
#ifdef PORTMIDI_ENABLED
	return pm_initialized_ && with_pm_mutex([] { return Pm_CountDevices(); }) > 0;
#else
	return false;
#endif
}

std::vector<std::pair<int, std::string>> MidiRouter::list_inputs() const {
	std::vector<std::pair<int, std::string>> out;
#ifdef PORTMIDI_ENABLED
	if (pm_initialized_) {
		// No Pm_CountInputDevices in 2.0.7: enumerate all devices and
		// filter by the input side of PmDeviceInfo.
		with_pm_mutex([&] {
			for (int id = 0; id < Pm_CountDevices(); ++id) {
				const PmDeviceInfo *info = Pm_GetDeviceInfo(id);
				if (info != nullptr && info->input != 0) {
					out.emplace_back(id, info->name != nullptr ? info->name : "");
				}
			}
		});
	}
#else
	(void)this;
#endif
	return out;
}

std::vector<std::pair<int, std::string>> MidiRouter::list_outputs() const {
	std::vector<std::pair<int, std::string>> out;
#ifdef PORTMIDI_ENABLED
	if (pm_initialized_) {
		with_pm_mutex([&] {
			for (int id = 0; id < Pm_CountDevices(); ++id) {
				const PmDeviceInfo *info = Pm_GetDeviceInfo(id);
				if (info != nullptr && info->output != 0) {
					out.emplace_back(id, info->name != nullptr ? info->name : "");
				}
			}
		});
	}
#else
	(void)this;
#endif
	return out;
}

// ---------------------------------------------------------------------------
// Control queue (API thread -> I/O thread)
// ---------------------------------------------------------------------------

MidiRouter::ControlHandle MidiRouter::enqueue_control(ControlOpType p_op, int p_pm_index,
		int p_port_id) {
	std::lock_guard<std::mutex> lock(control_mutex_);
	if (!thread_running_) {
		return ControlHandle{}; // no I/O thread (stub build, or PM init failed)
	}
	auto abandoned = std::make_shared<std::atomic<bool>>(false);
	auto done = std::make_shared<std::promise<int>>();
	ControlOp op;
	op.op = p_op;
	op.pm_index = p_pm_index;
	op.port_id = p_port_id;
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
		return; // idempotent; no thread (stub build, or PM init failed)
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
		// A wedged PM driver can hold the thread; surface it per spec §3
		// and join anyway (v1 platforms deliver output immediately, so
		// this is a documented residual risk, not the expected path).
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
#ifdef PORTMIDI_ENABLED
	PmEvent events[kPmReadBatch];
	for (;;) {
		if (process_control_ops()) {
			break; // SHUTDOWN handled: exit path closes everything
		}
		// Snapshot the open input ports (stream pointers are valid for
		// the whole loop: ports are only opened/closed on this thread).
		std::vector<int> input_ports;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &kv : ports_) {
				if (kv.second->in_use && kv.second->is_input &&
						kv.second->pm_stream != nullptr) {
					input_ports.push_back(kv.first);
				}
			}
		}
		// Stage 1 — read: Pm_Read + copy raw bytes into the per-port
		// rings (pinned: the read stage does no parsing).
		for (int pid : input_ports) {
			Port *port = port_ptr(pid);
			if (port == nullptr || port->pm_stream == nullptr) {
				continue;
			}
			PortMidiStream *stream = port->pm_stream;
			const int n = with_pm_mutex([&] {
				return Pm_Read(stream, events, kPmReadBatch);
			});
			if (n < 0) {
				handle_read_error(pid, n);
				continue;
			}
			for (int i = 0; i < n; ++i) {
				const uint32_t m = events[i].message;
				if (m & kPmLongFlag) {
					// Long (sysex) event: all four bytes are stream
					// bytes (F0/F7 included) — the framer re-frames them.
					port->ring.push(static_cast<uint8_t>(m & 0xFF));
					port->ring.push(static_cast<uint8_t>((m >> 8) & 0xFF));
					port->ring.push(static_cast<uint8_t>((m >> 16) & 0xFF));
					port->ring.push(static_cast<uint8_t>((m >> 24) & 0xFF));
				} else {
					// Short event: status + 0-2 data bytes, low byte first.
					const int bytes = pm_short_bytes(m);
					for (int b = 0; b < bytes; ++b) {
						port->ring.push(static_cast<uint8_t>((m >> (8 * b)) & 0xFF));
					}
				}
			}
			// Asynchronous host-error check (same loop, per stream).
			const int host_err = with_pm_mutex([&] {
				return Pm_HasHostError(stream);
			});
			if (host_err != 0) {
				// Pm_GetHostErrorText fills the caller's buffer (void);
				// the message is best-effort when a host error is set.
				char buf[256] = "";
				// Text gathered under the PM lock; notify_port_error
				// (user callback) fires after the lock is released.
				const char *text = with_pm_mutex([&] {
					Pm_GetHostErrorText(buf, sizeof(buf));
					return buf[0] != '\0' ? buf
							: Pm_GetErrorText(static_cast<PmError>(host_err));
				});
				notify_port_error(pid, text);
			}
		}
		// Stage 2 — frame + fan out (same thread, same iteration as the
		// read above; no parsing ever happens off this thread).
		for (int pid : input_ports) {
			Port *port = port_ptr(pid);
			if (port == nullptr || port->input == nullptr) {
				continue;
			}
			MidiFramer &framer = port->input->framer;
			port->ring.drain([&framer](uint8_t b) { framer.feed(b); });
		}
		// Stage 3 — output: drain instance queues into routed ports.
		output_stage();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Exit: close every remaining stream on this thread, then release PM.
	std::vector<PortMidiStream *> streams;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : ports_) {
			Port &p = *kv.second;
			if (p.in_use && p.pm_stream != nullptr) {
				streams.push_back(p.pm_stream);
				p.in_use = false;
				p.pm_stream = nullptr;
			}
		}
	}
	with_pm_mutex([&] {
		for (PortMidiStream *s : streams) {
			Pm_Close(s); // best effort
		}
	});
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
	if (pm_initialized_) {
		with_pm_mutex([] { Pm_Terminate(); }); // exactly once; this router
		// is the process's only PortMIDI user, so its lifetime bounds
		// PM use.
	}
#else
	// Stub build: the I/O thread never starts (no PM), nothing to do.
	(void)0;
#endif
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

int MidiRouter::open_port(bool p_is_input, int p_pm_index) {
#ifdef PORTMIDI_ENABLED
	PortMidiStream *stream = nullptr;
	// latency 0: deliver output immediately, no timestamp handling
	// (time_proc null -> PM's own time source; irrelevant at latency 0).
	PmError err;
	if (p_is_input) {
		err = with_pm_mutex([&] {
			return Pm_OpenInput(&stream, p_pm_index, nullptr, kPmBufferEvents,
					nullptr, nullptr);
		});
	} else {
		err = with_pm_mutex([&] {
			return Pm_OpenOutput(&stream, p_pm_index, nullptr, kPmBufferEvents,
					nullptr, nullptr, 0);
		});
	}
	if (err == pmNoError) {
		int port_id = -1;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			port_id = next_port_id_++;
			auto port = std::make_unique<Port>();
			port->in_use = true;
			port->is_input = p_is_input;
			port->pm_index = p_pm_index;
			port->pm_stream = stream;
			if (p_is_input) {
				port->input = new PortInput(this, port_id);
			}
			ports_[port_id] = std::move(port);
		}
		return port_id;
	}
	// Error text gathered under the PM lock; the user callback fires
	// after it is released.
	const std::string text = with_pm_mutex(
			[&] { return std::string(Pm_GetErrorText(err)); }) + " (pm device " +
		std::to_string(p_pm_index) + ")";
	notify_port_error(-1, text.c_str());
	return -1;
#else
	(void)p_is_input;
	(void)p_pm_index;
	return -1;
#endif
}

void MidiRouter::close_port(int p_port_id) {
#ifdef PORTMIDI_ENABLED
	PortMidiStream *stream = nullptr;
	bool is_input = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = ports_.find(p_port_id);
		if (it == ports_.end() || !it->second->in_use) {
			return; // unknown port, or already closed
		}
		stream = it->second->pm_stream;
		is_input = it->second->is_input;
	}
	if (stream != nullptr) {
		// Best effort; the stream is ours (I/O thread).
		with_pm_mutex([&] {
			Pm_Close(stream);
		});
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
			p.pm_stream = nullptr;
			p.ring.reset();
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
#else
	(void)p_port_id;
#endif
}

void MidiRouter::handle_read_error(int p_port_id, int p_err) {
#ifdef PORTMIDI_ENABLED
	if (p_err == pmBufferOverflow) {
		// PM flushed its input buffer on overflow. Drop the partial ring
		// and reset the framer so no torn message is decoded; keep the
		// port open (per Pm_Read's docs, ordinary processing resumes as
		// soon as a new message arrives).
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = ports_.find(p_port_id);
			if (it != ports_.end()) {
				it->second->ring.reset();
				if (it->second->input != nullptr) {
					it->second->input->framer.reset();
				}
			}
		}
		notify_port_error(p_port_id,
			with_pm_mutex([] { return Pm_GetErrorText(pmBufferOverflow); }));
		return;
	}
	// pmDeviceRemoved / pmHostError / anything else: the port is dead —
	// drop the handle, unroute, and notify.
	const std::string text = with_pm_mutex(
			[&] { return std::string(Pm_GetErrorText(static_cast<PmError>(p_err))); });
	close_port(p_port_id);
	notify_port_error(p_port_id, text.c_str());
#else
	(void)p_port_id;
	(void)p_err;
#endif
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
#ifdef PORTMIDI_ENABLED
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
			PortMidiStream *stream = nullptr;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto pit = ports_.find(port_id);
				if (pit != ports_.end() && pit->second->in_use &&
						pit->second->pm_stream != nullptr) {
					stream = pit->second->pm_stream;
				}
			}
			if (stream == nullptr) {
				continue; // port closed in the meantime: drop the messages
			}
			// No router lock during delivery: ports_ and raw_writers_
			// are I/O-thread-owned; Pm_WriteShort is never made under a
			// router lock (see the lock discipline in midi_router.h).
			auto &writers = raw_writers_[instance];
			for (const MidiOutMsg &msg : msgs) {
				if (!deliver_out_msg(stream, port_id, writers, msg)) {
					// Write failed: write_short already closed and
					// unrouted the port (its stream and raw-byte
					// framing state are gone) — drop the rest of this
					// port's batch; writing to the closed stream would
					// be UB.
					break;
				}
			}
		}
	}
#else
	(void)0;
#endif
}

#ifdef PORTMIDI_ENABLED
bool MidiRouter::deliver_out_msg(PortMidiStream *p_stream, int p_port_id,
		std::unordered_map<int, RawRouteState> &p_writers,
		const MidiOutMsg &p_msg) {
	if (p_msg.kind == MidiOutMsg::RAW_BYTE) {
		return raw_byte_to_stream(p_stream, p_port_id, p_writers, p_msg.byte);
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
	}
	return write_short(p_stream, p_port_id, Pm_Message(status, d1, d2));
}

bool MidiRouter::raw_byte_to_stream(PortMidiStream *p_stream, int p_port_id,
		std::unordered_map<int, RawRouteState> &p_writers,
		uint8_t p_byte) {
	RawRouteState &st = p_writers[p_port_id]; // default-constructed per route
	// The pinned MidiOutWriter transform (its per-call return is the wire
	// bytes for this input byte). The framing below mirrors that state
	// machine in lockstep — same transitions — and additionally tracks
	// message completion, because Pm_WriteShort needs full-form
	// PmMessage words (running-status input is re-expanded here).
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
			st.in_sysex = true; // raw F0..F7 dropped (no sysex output path)
			return true;
		}
		if (b >= 0xF8) {
			// Realtime: complete one-byte message, always sent (never
			// subject to running status).
			return write_short(p_stream, p_port_id, Pm_Message(b, 0, 0));
		}
		// Status byte (emitted, or running-suppressed — the wire status
		// is st.status, which equals b in the suppressed case) starts a
		// new message; any incomplete previous message is dropped.
		st.status = b;
		st.have_status = true;
		st.data_need = raw_data_need(b);
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
	// Message complete: send the full-form word.
	const uint8_t d2 = (st.data_need == 2) ? st.data[1] : 0;
	return write_short(p_stream, p_port_id, Pm_Message(st.status, st.data[0], d2));
}

bool MidiRouter::write_short(PortMidiStream *p_stream, int p_port_id, PmMessage p_msg) {
	const PmError err = with_pm_mutex(
			[&] { return Pm_WriteShort(p_stream, 0, p_msg); });
	if (err != pmNoError) {
		// Spec §7 (approved mapping: any nonzero PM error, notably
		// pmDeviceRemoved/pmHostError): drop the handle, unroute the
		// port, and notify. Pm_* calls are never made under a router
		// lock; close_port takes the locks it needs on its own.
		//
		// The port is dead now: returning false makes output_stage drop
		// the rest of the batch (writing to the closed stream would be
		// UB). Error text is gathered under the PM lock; the user
		// callback fires after it is released.
		const std::string text = with_pm_mutex(
				[&] { return std::string(Pm_GetErrorText(err)); });
		close_port(p_port_id);
		notify_port_error(p_port_id, text.c_str());
		return false;
	}
	return true;
}
#endif // PORTMIDI_ENABLED

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

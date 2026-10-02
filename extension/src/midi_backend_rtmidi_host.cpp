// RtMidi HOST backend (v2 M3, Task 1) — see midi_backend_rtmidi_host.h.
//
// CoreMIDI (macOS) + ALSA (Linux) behind the MidiBackend interface.
// Compiled only for non-Android hosts with RTMIDI_HOST_ENABLED (the
// factory in midi_backend_factory.cpp picks the backend per platform;
// MIDI_BACKEND=portmidi restores the M1/M2 PortMIDI path).
//
// Recon evidence (M3 Task 0, spec §7): probes/rtmidi_probe_macos.cpp
// (IAC enumerate/open/note+full-sysex round-trip, openVirtualPort
// both directions) and probes/rtmidi_probe_linux.cpp (A133, kernel
// 4.9: openVirtualPort both directions + aconnect loopback with note
// and 7-byte sysex).

#if !defined(__ANDROID__) && defined(RTMIDI_HOST_ENABLED)

#include "midi_backend_rtmidi_host.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace godot_libpd {

namespace {

// The client name registered with the platform MIDI server (shows up
// in `aconnect -l` on the A133 as the owning client of the virtual
// ports; the port names "libpd test app in 0" / "out 0" are the
// unambiguous identifiers).
constexpr const char *kClientName = "godot-libpd";

} // namespace

RtMidi::Api RtMidiHostBackend::api() {
#if defined(__APPLE__)
	return RtMidi::MACOSX_CORE;
#else
	return RtMidi::LINUX_ALSA;
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RtMidiHostBackend::~RtMidiHostBackend() {
	// The router shuts down before destroying the backend; this is a
	// belt-and-braces close for anything left open.
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &kv : in_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at teardown
			}
		}
	}
	for (auto &kv : out_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at teardown
			}
		}
	}
}

bool RtMidiHostBackend::available() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return initialized_;
}

MidiError RtMidiHostBackend::initialize() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_) {
		return MidiError::OK; // defensive: the router initializes once
	}
	// Probe the platform API once up front: constructing the objects
	// brings up the server client (CoreMIDI client ref / ALSA seq
	// client). A throw means the MIDI server is unavailable (no
	// audio on a headless box) — degrade to "backend unavailable"
	// exactly like a failed Pm_Initialize did.
	try {
		std::unique_ptr<RtMidiIn> probe_in(std::make_unique<RtMidiIn>(
				api(), kClientName, 100));
		std::unique_ptr<RtMidiOut> probe_out(
				std::make_unique<RtMidiOut>(api(), kClientName));
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		return MidiError::Unavailable;
	}
	initialized_ = true;
	return MidiError::OK;
}

std::vector<MidiBackendPort> RtMidiHostBackend::list_ports_unlocked() const {
	std::vector<MidiBackendPort> out;
	if (!initialized_) {
		return out; // pre-list behavior: failed init -> empty list
	}
	// Fresh per-side enumeration, paired into one unified device list:
	// position i on both sides with equal names is one device (IAC bus:
	// same position + same name on both sides); leftover positions are
	// one-side devices. (RtMidi's per-side lists preserve the platform
	// device order, so equal positions pair correctly.)
	std::vector<std::string> in_names;
	std::vector<std::string> out_names;
	{
		std::unique_ptr<RtMidiIn> probe_in(std::make_unique<RtMidiIn>(
				api(), kClientName, 100));
		const int n_in = probe_in->getPortCount();
		in_names.reserve(static_cast<size_t>(n_in));
		for (int i = 0; i < n_in; ++i) {
			in_names.push_back(probe_in->getPortName(i));
		}
	}
	{
		std::unique_ptr<RtMidiOut> probe_out(
				std::make_unique<RtMidiOut>(api(), kClientName));
		const int n_out = probe_out->getPortCount();
		out_names.reserve(static_cast<size_t>(n_out));
		for (int i = 0; i < n_out; ++i) {
			out_names.push_back(probe_out->getPortName(i));
		}
	}
	const int n = static_cast<int>(in_names.size() > out_names.size()
			? in_names.size()
			: out_names.size());
	devices_.clear();
	for (int i = 0; i < n; ++i) {
		const bool has_in = i < static_cast<int>(in_names.size());
		const bool has_out = i < static_cast<int>(out_names.size());
		// Pair equal positions only when the names agree (the IAC
		// case); otherwise each position is its own one-side device.
		if (has_in && has_out && in_names[i] == out_names[i]) {
			Device d;
			d.name = in_names[i];
			d.is_input = true;
			d.is_output = true;
			devices_.push_back(std::move(d));
		} else {
			if (has_in) {
				Device d;
				d.name = in_names[i];
				d.is_input = true;
				devices_.push_back(std::move(d));
			}
			if (has_out) {
				Device d;
				d.name = out_names[i];
				d.is_output = true;
				devices_.push_back(std::move(d));
			}
		}
	}
	for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
		MidiBackendPort port;
		port.index = i;
		port.name = devices_[i].name;
		port.is_input = devices_[i].is_input;
		port.is_output = devices_[i].is_output;
		out.push_back(port);
	}
	if (loopback_active_) {
		MidiBackendPort in;
		in.index = kLoopbackInputIndex;
		in.name = loopback_name_ + " in";
		in.is_input = true;
		out.push_back(in);
		MidiBackendPort outport;
		outport.index = kLoopbackOutputIndex;
		outport.name = loopback_name_ + " out";
		outport.is_output = true;
		out.push_back(outport);
	}
	return out;
}

std::vector<MidiBackendPort> RtMidiHostBackend::list_ports() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return list_ports_unlocked();
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void RtMidiHostBackend::input_callback(double p_time,
		std::vector<unsigned char> *p_bytes, void *p_userdata) {
	// RtMidi's pollMidi pthread. Messages arrive COMPLETE on these
	// APIs (short: status + data; sysex: the full F0..F7 — verified
	// by the recon probes). Chopped into <= 4-byte words for the
	// router's read stage (same layout PortMidi delivers via PmEvent
	// words; the read stage reassembles, sysex included).
	(void)p_time;
	auto *ring = static_cast<rtmidi_seam::WordRing *>(p_userdata);
	if (ring == nullptr || p_bytes == nullptr) {
		return;
	}
	rtmidi_seam::chop_to_words(p_bytes->data(), static_cast<int>(p_bytes->size()),
			[p_ring = ring](uint32_t w) { p_ring->push(w); });
}

int RtMidiHostBackend::find_side_position(bool p_is_input,
		const std::string &p_name) const {
	// Re-enumerate on open: the position of a device name can shift
	// (hot-plug, or a virtual port created since the last
	// list_ports()). Return the first match; -1 when gone.
	try {
		if (p_is_input) {
			std::unique_ptr<RtMidiIn> probe(std::make_unique<RtMidiIn>(
					api(), kClientName, 100));
			const int n = probe->getPortCount();
			for (int i = 0; i < n; ++i) {
				if (probe->getPortName(i) == p_name) {
					return i;
				}
			}
		} else {
			std::unique_ptr<RtMidiOut> probe(
					std::make_unique<RtMidiOut>(api(), kClientName));
			const int n = probe->getPortCount();
			for (int i = 0; i < n; ++i) {
				if (probe->getPortName(i) == p_name) {
					return i;
				}
			}
		}
	} catch (const RtMidiError &) {
		// enumeration blip: report "gone", the caller composes the
		// user-facing message.
	}
	return -1;
}

MidiError RtMidiHostBackend::open_impl(int p_index, int p_buffer_events,
		bool p_is_input, PortHandle &r_handle) {
	r_handle = NO_HANDLE;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	// In-process loopback pair (synchronous; no device open needed) —
	// identical to the Android backend.
	if (p_index == kLoopbackInputIndex) {
		if (!loopback_active_ || !p_is_input) {
			last_error_ = "loopback input not available";
			return MidiError::InvalidParameter;
		}
		if (loopback_in_open_) {
			last_error_ = "loopback input already open";
			return MidiError::Failed;
		}
		loopback_in_handle_ = next_handle_++;
		loopback_in_open_ = true;
		r_handle = loopback_in_handle_;
		return MidiError::OK;
	}
	if (p_index == kLoopbackOutputIndex) {
		if (!loopback_active_ || p_is_input) {
			last_error_ = "loopback output not available";
			return MidiError::InvalidParameter;
		}
		if (loopback_out_open_) {
			last_error_ = "loopback output already open";
			return MidiError::Failed;
		}
		loopback_out_handle_ = next_handle_++;
		loopback_out_open_ = true;
		r_handle = loopback_out_handle_;
		return MidiError::OK;
	}
	// Reserved openVirtualPort() device (create-then-open contract).
	auto pit = pending_virtual_.find(p_index);
	if (pit != pending_virtual_.end()) {
		const std::string &vname = pit->second.first;
		const bool reserved_input = pit->second.second;
		if (reserved_input != p_is_input) {
			last_error_ = "MIDI device " + std::to_string(p_index) +
					" (" + vname + ") was created for the " +
					(reserved_input ? "input" : "output") + " side only";
			return MidiError::Failed;
		}
		// Already opened? (two opens of one reserved device)
		for (const auto &kv : in_ports_) {
			if (kv.second != nullptr && kv.second->device_index == p_index) {
				last_error_ = "virtual MIDI input already open";
				return MidiError::Failed;
			}
		}
		for (const auto &kv : out_ports_) {
			if (kv.second != nullptr && kv.second->device_index == p_index) {
				last_error_ = "virtual MIDI output already open";
				return MidiError::Failed;
			}
		}
		// openVirtualPort() is the create-AND-open step on these
		// APIs (verified both directions by the recon probes); it is
		// synchronous, so no settle wait.
		PortHandle handle = next_handle_++;
		try {
			if (p_is_input) {
				auto rt_in =
						std::unique_ptr<RtMidiIn>(new RtMidiIn(api(),
								kClientName, static_cast<unsigned int>(p_buffer_events)));
				rt_in->ignoreTypes(false, false, false);
				auto in = std::unique_ptr<InPort>(new InPort{std::move(rt_in),
						p_index, vname,
						rtmidi_seam::WordRing(kInputRingCapacity)});
				in->rt->setCallback(&RtMidiHostBackend::input_callback,
						&in->ring);
				in->rt->openVirtualPort(vname);
				in_ports_.emplace(handle, std::move(in));
			} else {
				auto rt_out = std::unique_ptr<RtMidiOut>(
						new RtMidiOut(api(), kClientName));
				auto outp = std::unique_ptr<OutPort>(new OutPort{
						std::move(rt_out), p_index, vname});
				outp->rt->openVirtualPort(vname);
				out_ports_.emplace(handle, std::move(outp));
			}
		} catch (const RtMidiError &e) {
			// Contract: a failed open deletes the reservation (the
			// virtual device was never registered, or the error left
			// nothing usable).
			pending_virtual_.erase(pit);
			last_error_ = std::string("RtMidi: ") + e.what();
			return MidiError::Failed;
		}
		r_handle = handle;
		return MidiError::OK;
	}
	// Real device.
	// Lazy first-time enumeration: open without a prior list_ports()
	// refreshes instead of failing (the A133 GUI flow lists first;
	// this keeps the contract usable from scripts too).
	if (p_index < 0 || p_index >= static_cast<int>(devices_.size())) {
		// Trigger a refresh and retry once (mutex_ already held).
		(void)list_ports_unlocked();
		if (p_index < 0 || p_index >= static_cast<int>(devices_.size())) {
			last_error_ = "unknown MIDI device index " + std::to_string(p_index) +
					" (re-list the ports; the device list may have changed)";
			return MidiError::InvalidParameter;
		}
	}
	if (p_is_input ? !devices_[p_index].is_input : !devices_[p_index].is_output) {
		last_error_ = "MIDI device " + std::to_string(p_index) +
				" (" + devices_[p_index].name + ") has no " +
				(p_is_input ? "input" : "output") + " port";
		return MidiError::Failed;
	}
	const std::string &dev_name = devices_[p_index].name;
	const int pos = find_side_position(p_is_input, dev_name);
	if (pos < 0) {
		last_error_ = "MIDI device " + std::to_string(p_index) + " (" +
				dev_name + ") disappeared before it could be opened "
				"(re-list the ports)";
		return MidiError::Failed;
	}
	// One RtMidi object per open port (openPort() opens exactly one
	// port per object on every API).
	PortHandle handle = next_handle_++;
	try {
		if (p_is_input) {
			auto rt_in = std::unique_ptr<RtMidiIn>(new RtMidiIn(api(),
					kClientName, static_cast<unsigned int>(p_buffer_events)));
			// Receive EVERYTHING (the default ignoreTypes(7) would
			// drop sysex, realtime and sensing — the read stage needs
			// all of them).
			rt_in->ignoreTypes(false, false, false);
			auto in = std::unique_ptr<InPort>(new InPort{std::move(rt_in),
					p_index, dev_name,
					rtmidi_seam::WordRing(kInputRingCapacity)});
			in->rt->setCallback(&RtMidiHostBackend::input_callback,
					&in->ring);
			in->rt->openPort(static_cast<unsigned int>(pos), dev_name);
			in_ports_.emplace(handle, std::move(in));
		} else {
			auto rt_out = std::unique_ptr<RtMidiOut>(
					new RtMidiOut(api(), kClientName));
			auto outp = std::unique_ptr<OutPort>(new OutPort{
					std::move(rt_out), p_index, dev_name});
			outp->rt->openPort(static_cast<unsigned int>(pos), dev_name);
			out_ports_.emplace(handle, std::move(outp));
		}
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		return MidiError::Failed;
	}
	// Synchronous open on CoreMIDI/ALSA: by the time openPort()
	// returns the port is live (no Android settle wait).
	r_handle = handle;
	return MidiError::OK;
}

MidiError RtMidiHostBackend::open_input(int p_index, int p_buffer_events,
		PortHandle &r_handle) {
	return open_impl(p_index, p_buffer_events, true, r_handle);
}

MidiError RtMidiHostBackend::open_output(int p_index, int p_buffer_events,
		PortHandle &r_handle) {
	return open_impl(p_index, p_buffer_events, false, r_handle);
}

// ---------------------------------------------------------------------------
// Input polling / stream health
// ---------------------------------------------------------------------------

MidiBackend::PollResult RtMidiHostBackend::poll_input(PortHandle p_handle,
		const WordPush &p_push) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (p_handle == loopback_in_handle_ && loopback_in_open_) {
		return loopback_ring_.drain(p_push) ? PollResult::BUFFER_OVERFLOW
				: PollResult::OK;
	}
	auto it = in_ports_.find(p_handle);
	if (it == in_ports_.end()) {
		return PollResult::OK; // defensive: the router stops polling on close
	}
	// No PollResult::FATAL on this backend: this RtMidi version has no
	// device-removal / fatal-stream signal on the host APIs (a dead
	// port simply stops delivering words — same stance as Android).
	return it->second->ring.drain(p_push) ? PollResult::BUFFER_OVERFLOW
			: PollResult::OK;
}

bool RtMidiHostBackend::has_host_error(PortHandle p_handle, std::string &r_text) {
	// RtMidi has no queryable per-stream error state: errors surface as
	// RtMidiError exceptions AT CALL TIME (openPort/openVirtualPort/
	// sendMessage), which open_impl()/write_locked() already convert to
	// Failed returns the router handles. Nothing to poll.
	(void)p_handle;
	(void)r_text;
	return false;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

MidiError RtMidiHostBackend::write(PortHandle p_handle, const uint8_t *p_bytes,
		int p_len) {
	std::lock_guard<std::mutex> lock(mutex_);
	return write_locked(p_handle, p_bytes, p_len);
}

MidiError RtMidiHostBackend::write_locked(PortHandle p_handle,
		const uint8_t *p_bytes, int p_len) {
	if (p_handle == loopback_out_handle_) {
		if (!loopback_out_open_) {
			last_error_ = "no open port";
			return MidiError::Failed;
		}
		// In-process loopback: the wire bytes are exactly the router's
		// full-form framing, chopped to words into the loopback
		// input's ring (identical layout to a real device delivery).
		if (p_len <= 0) {
			return MidiError::OK;
		}
		rtmidi_seam::chop_to_words(p_bytes, p_len,
				[&](uint32_t w) { loopback_ring_.push(w); });
		return MidiError::OK;
	}
	auto it = out_ports_.find(p_handle);
	if (it == out_ports_.end()) {
		last_error_ = "no open port";
		return MidiError::Failed;
	}
	// A throw here (device removed / port died) is the fatal path: the
	// router's write-failure handling closes the port and notifies
	// (spec §7 — same contract as the Android backend).
	try {
		it->second->rt->sendMessage(p_bytes, static_cast<size_t>(p_len));
	} catch (const RtMidiError &e) {
		last_error_ = std::string("RtMidi: ") + e.what();
		return MidiError::Failed;
	}
	return MidiError::OK;
}

MidiError RtMidiHostBackend::write_sysex(PortHandle p_handle,
		const uint8_t *p_bytes, int p_len) {
	// Guard the payload shape first (mirrors the PortMidi backend);
	// then the same path as short messages (CoreMIDI/ALSA accept the
	// full F0..F7 stream as one message — recon-verified).
	if (p_len < 2 || p_bytes[0] != 0xF0 || p_bytes[p_len - 1] != 0xF7) {
		std::lock_guard<std::mutex> lock(mutex_);
		last_error_ = "malformed sysex (must start with 0xF0 and end with 0xF7)";
		return MidiError::InvalidParameter;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	return write_locked(p_handle, p_bytes, p_len);
}

// ---------------------------------------------------------------------------
// Close / virtual devices / shutdown
// ---------------------------------------------------------------------------

MidiError RtMidiHostBackend::close(PortHandle p_handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (p_handle == loopback_in_handle_) {
		if (loopback_in_open_) {
			loopback_in_open_ = false;
			loopback_ring_.reset();
		}
		return MidiError::OK; // best effort (nothing to clean)
	}
	if (p_handle == loopback_out_handle_) {
		loopback_out_open_ = false;
		return MidiError::OK;
	}
	auto in = in_ports_.find(p_handle);
	if (in != in_ports_.end()) {
		const int device_index = in->second != nullptr
				? in->second->device_index
				: -1;
		// closePort() also deletes the virtual device on these APIs
		// (it joins the pollMidi thread first, which pushes into this
		// InPort's ring — alive until the erase below).
		if (in->second != nullptr) {
			try {
				in->second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort (spec: close never fails loudly)
			}
		}
		in_ports_.erase(in);
		if (device_index >= kVirtualIndexBase) {
			pending_virtual_.erase(device_index);
		}
		return MidiError::OK;
	}
	auto out = out_ports_.find(p_handle);
	if (out != out_ports_.end()) {
		const int device_index = out->second != nullptr
				? out->second->device_index
				: -1;
		if (out->second != nullptr) {
			try {
				out->second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort
			}
		}
		out_ports_.erase(out);
		if (device_index >= kVirtualIndexBase) {
			pending_virtual_.erase(device_index);
		}
		return MidiError::OK;
	}
	return MidiError::OK; // best effort: unknown handle is a no-op
}

MidiError RtMidiHostBackend::create_virtual_input(const std::string &p_name,
		int &r_index) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	// Reserve the internal index; the port itself is created (and
	// registered with the platform server) at the open step — the
	// create-then-open contract of MidiBackend (the router calls
	// open_input() with the returned index right after).
	const int index = next_virtual_index_++;
	pending_virtual_[index] = std::make_pair(p_name, true);
	r_index = index;
	return MidiError::OK;
}

MidiError RtMidiHostBackend::create_virtual_output(const std::string &p_name,
		int &r_index) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	const int index = next_virtual_index_++;
	pending_virtual_[index] = std::make_pair(p_name, false);
	r_index = index;
	return MidiError::OK;
}

MidiError RtMidiHostBackend::create_virtual_loopback(
		const std::string &p_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) {
		last_error_ = "RtMidi: backend not initialized";
		return MidiError::Failed;
	}
	if (loopback_active_) {
		last_error_ = "an in-process loopback is already active";
		return MidiError::Failed;
	}
	loopback_active_ = true;
	loopback_name_ = p_name;
	loopback_ring_.reset();
	return MidiError::OK;
}

void RtMidiHostBackend::shutdown() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &kv : in_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at shutdown
			}
		}
	}
	in_ports_.clear();
	for (auto &kv : out_ports_) {
		if (kv.second != nullptr) {
			try {
				kv.second->rt->closePort();
			} catch (const RtMidiError &) {
				// best effort at shutdown
			}
		}
	}
	out_ports_.clear();
	// Never-opened reservations: the virtual device was never
	// registered (create only reserved the index), so there is
	// nothing to delete.
	pending_virtual_.clear();
	loopback_active_ = false;
	loopback_in_open_ = false;
	loopback_out_open_ = false;
	loopback_ring_.reset();
	initialized_ = false;
}

const std::string &RtMidiHostBackend::last_error() const {
	return last_error_;
}

} // namespace godot_libpd

#endif // !__ANDROID__

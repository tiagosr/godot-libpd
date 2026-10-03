// PortMidi backend (v2 M2, Task 1) — see midi_backend_portmidi.h.
//
// Behavior is a straight extraction of the Pm_* logic that lived in
// midi_router.cpp before Task 1: same bounds validation (A133 freeze
// repro note kept in open_stream), same error text composition
// (including the " (pm device N)" open-failure suffix), same buffer
// sizes, same shutdown ordering (close streams -> delete owned
// virtual devices -> Pm_Terminate, exactly once, on the I/O thread).

#include "midi_backend_portmidi.h"

#ifdef PORTMIDI_ENABLED

#include <cstdio>
#include <utility>

namespace godot_libpd {

namespace {
// Pm_Read batch per poll (one ~1 ms I/O-loop iteration).
constexpr int kPmReadBatch = 32;
} // namespace

bool PortMidiBackend::available() const {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	return initialized_;
}

MidiError PortMidiBackend::initialize() {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	if (initialized_) {
		return MidiError::OK; // defensive: the router initializes once
	}
	const PmError err = Pm_Initialize();
	if (err != pmNoError) {
		last_error_ = Pm_GetErrorText(err);
		return MidiError::Failed;
	}
	initialized_ = true;
	return MidiError::OK;
}

std::vector<MidiBackendPort> PortMidiBackend::list_ports() const {
	std::vector<MidiBackendPort> out;
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	if (!initialized_) {
		return out; // pre-extraction behavior: failed init -> empty list
	}
	// No Pm_CountInput/OutputDevices in 2.0.7: enumerate all devices
	// and flag the sides; the router filters per list_inputs()/
	// list_outputs().
	for (int id = 0; id < Pm_CountDevices(); ++id) {
		const PmDeviceInfo *info = Pm_GetDeviceInfo(id);
		if (info == nullptr || (info->input == 0 && info->output == 0)) {
			continue;
		}
		MidiBackendPort port;
		port.index = id;
		port.name = (info->name != nullptr) ? info->name : "";
		port.is_input = info->input != 0;
		port.is_output = info->output != 0;
		out.push_back(port);
	}
	return out;
}

PmError PortMidiBackend::open_stream(int p_index, int p_buffer_events, bool p_is_input,
		PortMidiStream *&r_stream) {
	// Validate the index AND open under one pm_api_mutex_ scope
	// (A133 freeze repro, carried over from midi_router.cpp): the
	// device list can shrink between the caller's list_*() and this
	// open, and the vendored Pm_OpenInput (unlike Pm_OpenOutput) has
	// no bounds check — an out-of-range index read pm_descriptors OOB
	// (UB: the A133 freeze). The 2.0.7 index space is
	// 0..Pm_CountDevices()-1; the side check keeps the existing
	// pmInvalidDeviceId rejection for a device without the requested
	// side.
	if (p_index < 0 || p_index >= Pm_CountDevices()) {
		return pmInvalidDeviceId;
	}
	const PmDeviceInfo *info = Pm_GetDeviceInfo(p_index);
	if (info == nullptr || (p_is_input ? info->input == 0 : info->output == 0)) {
		return pmInvalidDeviceId;
	}
	// latency 0: deliver output immediately, no timestamp handling
	// (time_proc null -> PM's own time source; irrelevant at latency 0).
	if (p_is_input) {
		return Pm_OpenInput(&r_stream, p_index, nullptr, p_buffer_events,
				nullptr, nullptr);
	}
	return Pm_OpenOutput(&r_stream, p_index, nullptr, p_buffer_events,
			nullptr, nullptr, 0);
}

MidiError PortMidiBackend::open_impl(int p_index, int p_buffer_events, bool p_is_input,
		PortHandle &r_handle) {
	r_handle = NO_HANDLE;
	PortMidiStream *stream = nullptr;
	const PmError err = open_stream(p_index, p_buffer_events, p_is_input, stream);
	if (err != pmNoError) {
		// This index may be a pending virtual device this backend
		// created: delete it so the failed open cannot leak it
		// (mirrors the old router open_virtual_port cleanup).
		if (pending_virtual_devices_.erase(p_index) != 0) {
			Pm_DeleteVirtualDevice(p_index);
		}
		last_error_ = std::string(Pm_GetErrorText(err)) + " (pm device " +
				std::to_string(p_index) + ")";
		return MidiError::Failed;
	}
	OpenPort op;
	op.stream = stream;
	op.device_index = p_index;
	const auto pit = pending_virtual_devices_.find(p_index);
	if (pit != pending_virtual_devices_.end()) {
		// The backend created that device; it now belongs to this
		// stream (close()/shutdown() delete it).
		op.device_owned = true;
		pending_virtual_devices_.erase(pit);
	}
	open_ports_[next_handle_] = std::move(op);
	r_handle = next_handle_++;
	return MidiError::OK;
}

MidiError PortMidiBackend::open_input(int p_index, int p_buffer_events, PortHandle &r_handle) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	return open_impl(p_index, p_buffer_events, /*p_is_input=*/true, r_handle);
}

MidiError PortMidiBackend::open_output(int p_index, int p_buffer_events, PortHandle &r_handle) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	return open_impl(p_index, p_buffer_events, /*p_is_input=*/false, r_handle);
}

MidiBackend::PollResult PortMidiBackend::poll_input(PortHandle p_handle, const WordPush &p_push) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	auto it = open_ports_.find(p_handle);
	if (it == open_ports_.end() || it->second.stream == nullptr) {
		last_error_ = "no open port";
		return PollResult::FATAL;
	}
	PortMidiStream *stream = it->second.stream;
	PmEvent events[kPmReadBatch];
	const int n = Pm_Read(stream, events, kPmReadBatch);
	if (n == pmBufferOverflow) {
		// PM flushed its input buffer on overflow. The router resets
		// the port's read state; the port stays open (per Pm_Read's
		// docs, ordinary processing resumes as soon as a new message
		// arrives).
		last_error_ = Pm_GetErrorText(pmBufferOverflow);
		return PollResult::BUFFER_OVERFLOW;
	}
	if (n < 0) {
		// pmDeviceRemoved / pmHostError / anything else: the stream is
		// dead.
		last_error_ = Pm_GetErrorText(static_cast<PmError>(n));
		return PollResult::FATAL;
	}
	for (int i = 0; i < n; ++i) {
		const uint8_t b0 = static_cast<uint8_t>(events[i].message & 0xFF);
		// PortMIDI re-inserts running status (full-form), so short events
		// are always status-first -> midi_short_bytes(b0) is exact. Sysex
		// arrives as 4-byte PmEvents (F0-start or data-first continuation),
		// which the read stage reassembles; report the full word (4).
		const int count = (b0 < 0x80 || b0 == 0xF0) ? 4 : midi_short_bytes(b0);
		p_push(events[i].message, count);
	}
	return PollResult::OK;
}

bool PortMidiBackend::has_host_error(PortHandle p_handle, std::string &r_text) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	auto it = open_ports_.find(p_handle);
	if (it == open_ports_.end() || it->second.stream == nullptr) {
		return false;
	}
	const int host_err = Pm_HasHostError(it->second.stream);
	if (host_err == 0) {
		return false;
	}
	// Pm_GetHostErrorText fills the caller's buffer (void); the message
	// is best-effort when a host error is set.
	char buf[256] = "";
	Pm_GetHostErrorText(buf, sizeof(buf));
	if (buf[0] != '\0') {
		r_text = buf;
	} else {
		r_text = Pm_GetErrorText(static_cast<PmError>(host_err));
	}
	return true;
}

MidiError PortMidiBackend::write(PortHandle p_handle, const uint8_t *p_bytes, int p_len) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	auto it = open_ports_.find(p_handle);
	if (it == open_ports_.end() || it->second.stream == nullptr) {
		last_error_ = "no open port";
		return MidiError::Failed;
	}
	// Full-form short message: {status, d1, d2}, low byte first in the
	// Pm_Message word (same word layout as the input PmEvent).
	const PmMessage word = Pm_Message(p_bytes[0], p_bytes[1],
			(p_len > 2) ? p_bytes[2] : 0);
	const PmError err = Pm_WriteShort(it->second.stream, 0, word);
	if (err != pmNoError) {
		last_error_ = Pm_GetErrorText(err);
		return MidiError::Failed;
	}
	return MidiError::OK;
}

MidiError PortMidiBackend::write_sysex(PortHandle p_handle, const uint8_t *p_bytes, int p_len) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	auto it = open_ports_.find(p_handle);
	if (it == open_ports_.end() || it->second.stream == nullptr) {
		last_error_ = "no open port";
		return MidiError::Failed;
	}
	// Pm_WriteSysEx scans until 0xF7 and takes a non-const buffer (API
	// wart); guard the payload shape first.
	if (p_len < 2 || p_bytes[0] != 0xF0 || p_bytes[p_len - 1] != 0xF7) {
		last_error_ = "malformed sysex (must start with 0xF0 and end with 0xF7)";
		return MidiError::InvalidParameter;
	}
	const PmError err = Pm_WriteSysEx(it->second.stream, 0,
			const_cast<unsigned char *>(p_bytes));
	if (err != pmNoError) {
		last_error_ = Pm_GetErrorText(err);
		return MidiError::Failed;
	}
	return MidiError::OK;
}

MidiError PortMidiBackend::close(PortHandle p_handle) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	auto it = open_ports_.find(p_handle);
	if (it == open_ports_.end()) {
		last_error_ = "no open port";
		return MidiError::Failed;
	}
	const OpenPort op = it->second;
	open_ports_.erase(it);
	if (op.stream != nullptr) {
		Pm_Close(op.stream); // best effort
	}
	// App-created virtual device: delete it now that the stream is
	// closed. Pm_Close alone does not remove it (ALSA's alsa_in_close
	// keeps virtual ports open on purpose — the port IS the device),
	// and Pm_DeleteVirtualDevice refuses an open device.
	if (op.device_owned) {
		Pm_DeleteVirtualDevice(op.device_index);
	}
	return MidiError::OK;
}

MidiError PortMidiBackend::create_virtual_input(const std::string &p_name, int &r_index) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	const int device_id = Pm_CreateVirtualInput(p_name.c_str(), nullptr, nullptr);
	if (device_id < 0) {
		last_error_ = Pm_GetErrorText(static_cast<PmError>(device_id));
		return MidiError::Failed;
	}
	r_index = device_id;
	pending_virtual_devices_.insert(device_id);
	return MidiError::OK;
}

MidiError PortMidiBackend::create_virtual_output(const std::string &p_name, int &r_index) {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	const int device_id = Pm_CreateVirtualOutput(p_name.c_str(), nullptr, nullptr);
	if (device_id < 0) {
		last_error_ = Pm_GetErrorText(static_cast<PmError>(device_id));
		return MidiError::Failed;
	}
	r_index = device_id;
	pending_virtual_devices_.insert(device_id);
	return MidiError::OK;
}

MidiError PortMidiBackend::create_virtual_loopback(const std::string &p_name) {
	// PortMidi has no in-process loopback; on macOS use IAC (Audio
	// MIDI Setup), on Linux aconnect. The Android RtMidi backend
	// implements this in-process (design spec §5).
	last_error_ = "virtual loopback unsupported by the PortMidi backend (use IAC or aconnect)";
	return MidiError::Unavailable;
}

void PortMidiBackend::shutdown() {
	std::lock_guard<std::mutex> lock(pm_api_mutex_);
	// Close streams first, then delete the virtual devices they own,
	// then any not-yet-opened virtual device, then terminate — the
	// order the old router's io_loop exit used (Pm_DeleteVirtualDevice
	// refuses an open device; Pm_Terminate exactly once, here).
	for (auto &kv : open_ports_) {
		OpenPort &op = kv.second;
		if (op.stream != nullptr) {
			Pm_Close(op.stream); // best effort
			op.stream = nullptr;
		}
		if (op.device_owned) {
			Pm_DeleteVirtualDevice(op.device_index); // best effort
		}
	}
	open_ports_.clear();
	for (int device_id : pending_virtual_devices_) {
		Pm_DeleteVirtualDevice(device_id); // best effort
	}
	pending_virtual_devices_.clear();
	if (initialized_) {
		Pm_Terminate();
		initialized_ = false;
	}
}

const std::string &PortMidiBackend::last_error() const {
	return last_error_;
}

} // namespace godot_libpd

#endif // PORTMIDI_ENABLED

// The platform factory (create_midi_backend) lives in
// midi_backend_factory.cpp — it selects PortMidiBackend on
// macOS/Linux (PORTMIDI_ENABLED), RtMidiAndroidBackend on Android
// (__ANDROID__), and nullptr on hosts without a platform backend.

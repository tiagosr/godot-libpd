// PortMidi backend (v2 M2, Task 1) — the concrete MidiBackend for
// macOS/Linux, built on the vendored PortMIDI 2.0.7. This class (and
// its .cpp) is the only place in the extension that touches Pm_*.
//
// Guarded on PORTMIDI_ENABLED so stub builds (BUILD_PORTMIDI=OFF) can
// still compile and link the factory, which then returns nullptr.

#pragma once

#ifdef PORTMIDI_ENABLED

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "midi_backend.h"

#include <portmidi.h>
#include <mutex>

namespace godot_libpd {

class PortMidiBackend : public MidiBackend {
public:
	PortMidiBackend() = default;
	~PortMidiBackend() override = default;

	// MidiBackend.
	bool available() const override;
	MidiError initialize() override;
	std::vector<MidiBackendPort> list_ports() const override;
	MidiError open_input(int p_index, int p_buffer_events, PortHandle &r_handle) override;
	MidiError open_output(int p_index, int p_buffer_events, PortHandle &r_handle) override;
	PollResult poll_input(PortHandle p_handle, const WordPush &p_push) override;
	MidiError write(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override;
	MidiError write_sysex(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override;
	MidiError close(PortHandle p_handle) override;
	MidiError create_virtual_input(const std::string &p_name, int &r_index) override;
	MidiError create_virtual_output(const std::string &p_name, int &r_index) override;
	MidiError create_virtual_loopback(const std::string &p_name) override;
	void shutdown() override;
	bool has_host_error(PortHandle p_handle, std::string &r_text) override;
	const std::string &last_error() const override;

private:
	struct OpenPort {
		PortMidiStream *stream = nullptr;
		int device_index = -1; // the Pm device id that was opened
		bool device_owned = false; // this backend created it (virtual)
	};

	// Assumes pm_api_mutex_ is held.
	MidiError open_impl(int p_index, int p_buffer_events, bool p_is_input, PortHandle &r_handle);
	// Assumes pm_api_mutex_ is held. Validate + open in one scope.
	PmError open_stream(int p_index, int p_buffer_events, bool p_is_input, PortMidiStream *&r_stream);

	bool initialized_ = false;

	// Guards every Pm_* call and the state below. PortMidi is not
	// thread-safe, and the router calls list_ports()/available() from
	// the API thread while the I/O thread polls.
	mutable std::mutex pm_api_mutex_;

	std::unordered_map<PortHandle, OpenPort> open_ports_;
	// Virtual devices created by this backend that are not yet open
	// (a successful open moves them into OpenPort.device_owned; a
	// failed open deletes them so failed opens cannot leak).
	std::unordered_set<int> pending_virtual_devices_;
	int next_handle_ = 0;
	std::string last_error_;
};

} // namespace godot_libpd

#endif // PORTMIDI_ENABLED

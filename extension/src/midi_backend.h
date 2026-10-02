// MIDI backend abstraction (v2 M2, Task 1 — design spec §3).
//
// The seam between the platform MIDI API and MidiRouter. MidiRouter
// talks to MidiBackend only; every concrete platform call (PortMidi
// Pm_* on macOS/Linux, RtMidi/AMIDI on Android) lives in one platform
// backend. Today the only built backend is PortMidiBackend (vendored
// PortMIDI 2.0.7); the Android RtMidi backend is a later v2 task.
//
// Conventions:
//  - *port index*: the backend's device index (PortMidi: Pm device
//    id, stable within a process). The router treats it as opaque.
//  - *port handle*: an opaque token the backend assigns per open
//    stream. NO_HANDLE means "no open port".
//  - *word*: up to 4 raw MIDI bytes packed low-byte-first into a
//    uint32_t — the same layout as the vendored PmEvent.message /
//    Pm_Message word. MidiReadStage reassembles words into a byte
//    stream (sysex included).
//  - error text: on a failed call the backend composes the full,
//    user-presentable message in last_error() (PortMidi open failures
//    include the " (pm device N)" suffix); the router surfaces it
//    verbatim on the port_error signal.
//  - threading: the router calls the backend from its I/O thread for
//    port operations; list_ports()/available() are const and may be
//    called from the API thread. last_error() is I/O-thread-only.
//    Backends keep their own locks for internal state.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace godot_libpd {

// Backend result codes (kept godot-free so the interface header stays
// pure and unit-test targets stay lightweight; the router maps to
// godot::Error at the boundary).
enum class MidiError {
	OK = 0,
	Failed,
	InvalidParameter,
	Unavailable,
};

// A port the backend can offer (input and/or output). `index` is the
// device index to pass back to open_input()/open_output().
struct MidiBackendPort {
	int index = -1;
	std::string name;
	bool is_input = false;
	bool is_output = false;
};

// Abstract platform MIDI backend: small, flat, one method per
// operation the router needs (design spec §3).
class MidiBackend {
public:
	using PortHandle = int;
	static constexpr PortHandle NO_HANDLE = -1;

	// Receives one <= 4-byte word (low byte first).
	using WordPush = std::function<void(uint32_t)>;

	enum class PollResult {
		OK, // words pushed (possibly none).
		BUFFER_OVERFLOW, // the backend dropped the input buffer
						// mid-message; the caller must reset the port's
						// read state, the port stays open.
		FATAL, // the stream is dead (device removed, fatal host
				// error); the caller should close the port,
				// last_error() carries the reason.
	};

	virtual ~MidiBackend() = default;

	// True once the backend is initialized.
	virtual bool available() const = 0;

	// One-time backend setup (PortMidi: Pm_Initialize).
	virtual MidiError initialize() = 0;

	// Ports the backend offers (empty while not initialized).
	virtual std::vector<MidiBackendPort> list_ports() const = 0;

	// Opens a port for input/output. On failure, last_error() carries
	// the full message (PortMidi includes " (pm device N)").
	virtual MidiError open_input(int p_index, int p_buffer_events, PortHandle &r_handle) = 0;
	virtual MidiError open_output(int p_index, int p_buffer_events, PortHandle &r_handle) = 0;

	// Non-blocking poll: pushes all pending words via p_push and
	// reports stream health (see PollResult).
	virtual PollResult poll_input(PortHandle p_handle, const WordPush &p_push) = 0;

	// True if the platform reports a non-fatal host error for this
	// stream; r_text receives the error text.
	virtual bool has_host_error(PortHandle p_handle, std::string &r_text) = 0;

	// Writes a full-form short message: p_bytes = {status, d1, d2},
	// p_len == 3 (realtime messages: {F8..FF, 0, 0}).
	virtual MidiError write(PortHandle p_handle, const uint8_t *p_bytes, int p_len) = 0;

	// Writes a full sysex: p_bytes[0] == 0xF0, p_bytes[p_len - 1] ==
	// 0xF7. Guarded: malformed input returns ERR_INVALID_PARAMETER.
	virtual MidiError write_sysex(PortHandle p_handle, const uint8_t *p_bytes, int p_len) = 0;

	// Closes the stream (and any virtual device it owns). Best effort.
	virtual MidiError close(PortHandle p_handle) = 0;

	// Creates a virtual input/output device; r_index receives the new
	// device index (openable via open_input/open_output). The backend
	// owns the device's lifetime: a failed open of the returned index
	// deletes it, and close()/shutdown() delete it with the stream.
	virtual MidiError create_virtual_input(const std::string &p_name, int &r_index) = 0;
	virtual MidiError create_virtual_output(const std::string &p_name, int &r_index) = 0;

	// In-process loopback facility: one input port and one output port
	// wired together, reported by list_ports() under a distinctive
	// name. PortMidi: MidiError::Unavailable (use IAC / aconnect).
	virtual MidiError create_virtual_loopback(const std::string &p_name) = 0;

	// Releases the backend: closes all open streams, deletes owned
	// virtual devices, terminates the platform API. I/O thread only.
	virtual void shutdown() = 0;

	// Full error text of the last failed backend call. I/O-thread-only.
	virtual const std::string &last_error() const = 0;
};

// Builds the platform backend:
//   macOS/Linux : PortMidiBackend (vendored PortMIDI 2.0.7)
//   ANDROID     : nullptr for now (RtMidi AMIDI backend, later v2 task)
//   other hosts : nullptr (no MIDI I/O; the router degrades to the
//                 inert stub of the pre-v2 builds)
// The returned backend is uninitialized: the router calls initialize()
// before starting its I/O thread.
std::unique_ptr<MidiBackend> create_midi_backend();

} // namespace godot_libpd

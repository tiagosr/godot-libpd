// RtMidi HOST backend (v2 M3, Task 1) — CoreMIDI (macOS) + ALSA (Linux).
//
// The non-Android half of the RtMidi backend family. It wraps the
// vendored RtMidi behind MidiBackend, reusing the platform-free seams
// (rtmidi_seam::chop_to_words / WordRing) that the Android backend
// (midi_backend_rtmidi.cpp) already uses — same callback -> word ring
// -> router read stage path, same in-process loopback contract.
//
// Differences vs the Android backend (see midi_backend_rtmidi.h):
//  - API selection is compile-time: MACOSX_CORE on Apple, LINUX_ALSA
//    on Linux (both verified by the M3 Task 0 recon probes —
//    probes/rtmidi_probe_macos.cpp, probes/rtmidi_probe_linux.cpp).
//  - Enumeration uses RtMidi's own per-side getPortCount()/
//    getPortName() (RtMidi is the single source; there is no
//    Android-style unified-index translation). The unified device
//    list is built by pairing the per-side entries: equal positions
//    with equal names pair into one device; leftover positions are
//    one-side devices.
//  - openPort() is SYNCHRONOUS on these APIs (no Android settle wait).
//  - openVirtualPort() EXISTS on these APIs (verified on macOS IAC
//    hosts and on the A133), so create_virtual_input/output are real:
//    the create step reserves an internal device index (300+), the
//    open step calls openVirtualPort() on a fresh RtMidi object. The
//    router's create-then-open contract (midi_router.cpp
//    open_virtual_port) is unchanged.
//  - No PollResult::FATAL: this RtMidi version has no queryable
//    device-removal / fatal-stream signal on the host APIs either
//    (same stance as the Android backend).
//
// Threading: the router I/O thread calls every method; RtMidi's
// pollMidi pthread delivers input through input_callback() into the
// per-port WordRing (the callback never touches a router lock).

#pragma once

#if !defined(__ANDROID__)

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "RtMidi.h"
#include "midi_backend.h"
// Platform-free seams (chop_to_words / WordRing) shared with the
// Android backend — defined in midi_backend_rtmidi.h outside its
// __ANDROID__ guard.
#include "midi_backend_rtmidi.h"

namespace godot_libpd {

class RtMidiHostBackend : public MidiBackend {
public:
	// MidiBackend.
	const char *backend_name() const override {
#if defined(__APPLE__)
		return "RtMidi(CoreMIDI)";
#else
		return "RtMidi(ALSA)";
#endif
	}

	// Unified indices for the in-process loopback pair (200 in / 201
	// out) — identical to the Android backend.
	static constexpr int kLoopbackInputIndex = 200;
	static constexpr int kLoopbackOutputIndex = 201;
	// Internal device indices for real openVirtualPort() ports (300+).
	// They live outside the enumerated device space (0..N) so a
	// re-enumeration can never alias them.
	static constexpr int kVirtualIndexBase = 300;

	~RtMidiHostBackend() override;

	bool available() const override;
	MidiError initialize() override;
	std::vector<MidiBackendPort> list_ports() const override;
	MidiError open_input(int p_index, int p_buffer_events, PortHandle &r_handle) override;
	MidiError open_output(int p_index, int p_buffer_events, PortHandle &r_handle) override;
	PollResult poll_input(PortHandle p_handle, const WordPush &p_push) override;
	bool has_host_error(PortHandle p_handle, std::string &r_text) override;
	MidiError write(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override;
	MidiError write_sysex(PortHandle p_handle, const uint8_t *p_bytes, int p_len) override;
	MidiError close(PortHandle p_handle) override;
	MidiError create_virtual_input(const std::string &p_name, int &r_index) override;
	MidiError create_virtual_output(const std::string &p_name, int &r_index) override;
	MidiError create_virtual_loopback(const std::string &p_name) override;
	void shutdown() override;
	const std::string &last_error() const override;

private:
	struct Device {
		std::string name;
		bool is_input = false;
		bool is_output = false;
	};
	struct InPort {
		std::unique_ptr<RtMidiIn> rt;
		int device_index = -1; // enumerated index, or 300+ for virtual
		std::string name;
		rtmidi_seam::WordRing ring;
	};
	struct OutPort {
		std::unique_ptr<RtMidiOut> rt;
		int device_index = -1;
		std::string name;
	};

	static void input_callback(double p_time, std::vector<unsigned char> *p_bytes,
			void *p_userdata);

	// Compile-time API selection (recon-verified; see file header).
	static RtMidi::Api api();

	MidiError open_impl(int p_index, int p_buffer_events, bool p_is_input,
			PortHandle &r_handle);
	// Write with mutex_ already held (write()/write_sysex() entry).
	MidiError write_locked(PortHandle p_handle, const uint8_t *p_bytes, int p_len);

	// list_ports() body without taking mutex_ (open_impl() refreshes
	// the device list while already holding it).
	std::vector<MidiBackendPort> list_ports_unlocked() const;

	// Fresh per-side enumeration; returns -1 when the device with
	// p_name is not present on the requested side (hot-removed).
	int find_side_position(bool p_is_input, const std::string &p_name) const;

	static constexpr int kLoopbackRingCapacity = 4096;
	static constexpr int kInputRingCapacity = 4096;

	mutable std::mutex mutex_;
	bool initialized_ = false;
	std::string last_error_;
	mutable std::vector<Device> devices_; // refreshed by list_ports()

	// Reserved (created, not yet opened) openVirtualPort() devices:
	// internal index -> (port name, side the reservation was created
	// for). The side is stored, not inferred from the index (in and
	// out reservations share the 300+ counter).
	std::unordered_map<int, std::pair<std::string, bool>> pending_virtual_;

	// unique_ptr so the map never moves the (non-movable-ring) port
	// state; the input callback's ring pointer is stable for the
	// port's whole life.
	std::unordered_map<PortHandle, std::unique_ptr<InPort>> in_ports_;
	std::unordered_map<PortHandle, std::unique_ptr<OutPort>> out_ports_;
	PortHandle next_handle_ = 0;
	int next_virtual_index_ = kVirtualIndexBase;

	bool loopback_active_ = false;
	std::string loopback_name_;
	bool loopback_in_open_ = false;
	bool loopback_out_open_ = false;
	PortHandle loopback_in_handle_ = NO_HANDLE;
	PortHandle loopback_out_handle_ = NO_HANDLE;
	// WordRing is non-copyable (mutex): initialized in place.
	rtmidi_seam::WordRing loopback_ring_{kLoopbackRingCapacity};
};

} // namespace godot_libpd

#endif // !__ANDROID__

// RtMidi Android backend (v2 M2, Task 2) — see midi_backend_rtmidi.cpp.
//
// Wraps the vendored RtMidi (pinned 759d4e6 — upstream 23b8cd5 plus the
// local ANDROID_AMIDI sysex fix) behind MidiBackend, __ANDROID__ only.
// On other platforms this header declares the pure, platform-free
// seams only; the factory (midi_backend_factory.cpp) selects the
// backend per platform.
//
// Android specifics (design spec 2026-10-01 §3/§5; device behavior
// verified in M2 Task 3/4):
//  - MidiManager.openDevice() is asynchronous and has NO failure
//    callback (the RtMidi listener only handles the success path).
//    open_input/open_output fire the open and settle-wait
//    kOpenSettleMs on the router I/O thread before returning OK
//    (that thread is not the Android main looper, so the open
//    completes while we wait). A failed open (e.g. busy device)
//    therefore surfaces as silent non-delivery, not as an error
//    return — documented platform limitation.
//  - RtMidi Android supports exactly ONE open port per
//    RtMidiIn/RtMidiOut object, so the backend keeps one RtMidi
//    object per open port.
//  - No virtual-MIDI-device API (RtMidi openVirtualPort is not
//    implemented for Android): create_virtual_input/output report
//    Unavailable; create_virtual_loopback is implemented in-process
//    (word ring from the loopback output handle to the loopback
//    input's ring — the same word layout real devices deliver).
//  - Device removal has no fatal signal in this RtMidi version:
//    poll_input simply stops receiving words (no PollResult::FATAL).

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "midi_backend.h"

#if defined(__ANDROID__)
#include <RtMidi.h>
#endif

namespace godot_libpd {

// ---------------------------------------------------------------------------
// Pure, platform-free seams (unit-tested on every host — see
// tests/midi_rtmidi_seam_tests.cpp). The Android .cpp uses exactly
// these; keeping them here means the word-chop/position/ring logic is
// verified without a device.
// ---------------------------------------------------------------------------

namespace rtmidi_seam {

// Chops a complete RtMidi message byte vector into <= 4-byte words
// (low byte first — the MidiBackend::WordPush layout) and pushes each
// via p_push. p_len <= 0 is a no-op. A 5-byte message yields words
// {b0..b3} then {b4}: the read stage reassembles the stream exactly
// (it only cares about <= 4 bytes per word).
inline void chop_to_words(const uint8_t *p_bytes, int p_len,
		const MidiBackend::WordPush &p_push) {
	for (int i = 0; i < p_len; i += 4) {
		uint32_t word = 0;
		for (int j = 0; j < 4 && i + j < p_len; ++j) {
			word |= static_cast<uint32_t>(p_bytes[i + j]) << (8 * j);
		}
		p_push(word);
	}
}

// Minimal device-side descriptor for the position math (the Android
// enumeration fills the same shape with names).
struct DeviceSide {
	bool input = false;
	bool output = false;
};

/**
 * Position of device p_index inside the SIDE-FILTERED device list
 * that RtMidi Android maintains (it keeps, in raw MidiManager order,
 * only the devices with > 0 ports on the requested side;
 * androidRefreshMidiDevices in RtMidi.cpp). RtMidi's openPort takes
 * the filtered position, while our unified MidiBackend index is the
 * raw position — this is the translation.
 *
 * Returns -1 when p_index is out of range or the device lacks the
 * requested side.
 */
inline int filtered_position(const std::vector<DeviceSide> &p_devices,
		int p_index, bool p_is_input) {
	if (p_index < 0 || p_index >= static_cast<int>(p_devices.size())) {
		return -1;
	}
	if (p_is_input ? !p_devices[p_index].input : !p_devices[p_index].output) {
		return -1;
	}
	int pos = 0;
	for (int i = 0; i < p_index; ++i) {
		if (p_is_input ? p_devices[i].input : p_devices[i].output) {
			++pos;
		}
	}
	return pos;
}

// Bounded word ring: producer = RtMidi's poll thread (input callback)
// or the router I/O thread (loopback output); consumer = the router
// I/O thread (poll_input). Overwrites are dropped; an overflow is
// reported once on the next drain (PollResult::BUFFER_OVERFLOW
// contract: the router resets the port's read state, the port stays
// open).
class WordRing {
public:
	WordRing() = default;
	explicit WordRing(int p_capacity) : cap_(p_capacity > 0 ? p_capacity : 1) {}

	// The ring moves (InPort is stored by value in a map); copies
	// are meaningless (mutex).
	WordRing(const WordRing &) = delete;
	WordRing &operator=(const WordRing &) = delete;
	WordRing(WordRing &&) = default;
	WordRing &operator=(WordRing &&) = default;

	void push(uint32_t p_word) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (words_.size() >= static_cast<size_t>(cap_)) {
			overflow_ = true; // drop
			return;
		}
		words_.push_back(p_word);
	}

	// Drains all pending words via p_take. Returns true if any push
	// overflowed since the last drain (sticky until reported).
	bool drain(const std::function<void(uint32_t)> &p_take) {
		std::lock_guard<std::mutex> lock(mutex_);
		const bool overflowed = overflow_;
		overflow_ = false;
		for (const uint32_t w : words_) {
			p_take(w);
		}
		words_.clear();
		return overflowed;
	}

	// Drops pending words + the overflow flag (port close/reopen).
	void reset() {
		std::lock_guard<std::mutex> lock(mutex_);
		words_.clear();
		overflow_ = false;
	}

private:
	std::mutex mutex_;
	std::deque<uint32_t> words_;
	int cap_ = 0;
	bool overflow_ = false;
};

} // namespace rtmidi_seam

#if defined(__ANDROID__)

class RtMidiAndroidBackend : public MidiBackend {
public:
	// Unified device indices for the in-process loopback pair
	// (200 in / 201 out; real devices enumerate 0..N).
	static constexpr int kLoopbackInputIndex = 200;
	static constexpr int kLoopbackOutputIndex = 201;

	~RtMidiAndroidBackend() override;

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
		int device_index = -1;
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

	MidiError open_impl(int p_index, int p_buffer_events, bool p_is_input,
			PortHandle &r_handle);
	// Write with mutex_ already held (write()/write_sysex() entry).
	MidiError write_locked(PortHandle p_handle, const uint8_t *p_bytes, int p_len);

	// Translate a unified (raw) device index into the side-filtered
	// position RtMidi's openPort expects (mirrors the > 0 ports
	// filter of androidRefreshMidiDevices, raw order preserved).
	static int side_position(const std::vector<Device> &p_devices, int p_index,
			bool p_is_input);

	// Settle wait after firing an asynchronous open (see header note).
	static constexpr int kOpenSettleMs = 300;
	static constexpr int kLoopbackRingCapacity = 4096;
	static constexpr int kInputRingCapacity = 4096;

	mutable std::mutex mutex_;
	bool initialized_ = false;
	std::string last_error_;
	mutable std::vector<Device> devices_; // refreshed by list_ports()

	// Unique_ptr so the map never moves the (non-movable-ring)
	// port state; the input callback's ring pointer is stable for
	// the port's whole life.
	std::unordered_map<PortHandle, std::unique_ptr<InPort>> in_ports_;
	std::unordered_map<PortHandle, std::unique_ptr<OutPort>> out_ports_;
	PortHandle next_handle_ = 0;

	bool loopback_active_ = false;
	std::string loopback_name_;
	bool loopback_in_open_ = false;
	bool loopback_out_open_ = false;
	PortHandle loopback_in_handle_ = NO_HANDLE;
	PortHandle loopback_out_handle_ = NO_HANDLE;
	// WordRing is non-copyable (mutex): initialized in place.
	rtmidi_seam::WordRing loopback_ring_{kLoopbackRingCapacity};
};

#endif // __ANDROID__

} // namespace godot_libpd

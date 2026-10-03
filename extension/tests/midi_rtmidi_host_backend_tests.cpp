// v2 M3 Task 1 — RtMidi host backend (CoreMIDI/ALSA) unit test.
//
// Covers the device-free contract surface: initialize() against the
// platform API, list_ports() shape, the in-process loopback
// (create_virtual_loopback -> open 200/201 -> write note + sysex ->
// poll words back, layout identical to real-device deliveries),
// write_sysex() shape guards, and shutdown().
//
// Hosts with no usable MIDI server (initialize() -> Unavailable)
// print "SKIP" and exit 0: headless CI without a MIDI server stays
// green. The real-device legs (macOS IAC smoke, A133 aconnect
// loopback) are the M3 Task 2 verification, run on hardware.
//
// Recon evidence that the platform APIs behave as this test assumes:
//   probes/rtmidi_probe_macos.cpp  (IAC: enumerate/open/note+sysex
//                                   round-trip, openVirtualPort both
//                                   directions)
//   probes/rtmidi_probe_linux.cpp  (A133 ALSA: openVirtualPort both
//                                   directions + aconnect loopback)

#include "midi_backend_rtmidi_host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace godot_libpd;

namespace {

int g_failures = 0;

void check(bool p_cond, const char *p_what) {
	if (p_cond) {
		std::printf("PASS %s\n", p_what);
	} else {
		std::printf("FAIL %s\n", p_what);
		g_failures++;
	}
}

// Collects the drained words of one poll_input call.
struct Collector {
	std::vector<uint32_t> words;
	void push(uint32_t w) { words.push_back(w); }
};

} // namespace

int main() {
	RtMidiHostBackend backend;

	if (backend.initialize() != MidiError::OK) {
		std::printf("SKIP no usable MIDI server on this host (%s)\n",
				backend.last_error().c_str());
		std::printf("HOST_BACKEND_SKIP\n");
		return 0; // exit 0: headless CI stays green
	}
	check(backend.available(), "available after initialize");
	std::printf("HOST_BACKEND_OK backend=%s\n", backend.backend_name());

	// --- list_ports shape ------------------------------------------------
	const std::vector<MidiBackendPort> ports = backend.list_ports();
	std::printf("list_ports: %zu device(s)\n", ports.size());
	for (const MidiBackendPort &p : ports) {
		check(p.index >= 0, "port has a non-negative index");
		check(p.is_input || p.is_output, "port has at least one side");
	}

	// --- in-process loopback round-trip ----------------------------------
	MidiError err = backend.create_virtual_loopback("m3-test-lb");
	check(err == MidiError::OK, "create_virtual_loopback");

	// The loopback pair shows up in the listing.
	const std::vector<MidiBackendPort> ports2 = backend.list_ports();
	bool seen_in = false;
	bool seen_out = false;
	for (const MidiBackendPort &p : ports2) {
		if (p.index == RtMidiHostBackend::kLoopbackInputIndex) {
			seen_in = true;
			check(p.is_input && !p.is_output, "loopback in is input-only");
		}
		if (p.index == RtMidiHostBackend::kLoopbackOutputIndex) {
			seen_out = true;
			check(p.is_output && !p.is_input, "loopback out is output-only");
		}
	}
	check(seen_in, "loopback input listed (index 200)");
	check(seen_out, "loopback output listed (index 201)");

	MidiBackend::PortHandle in_handle = MidiBackend::NO_HANDLE;
	MidiBackend::PortHandle out_handle = MidiBackend::NO_HANDLE;
	check(backend.open_input(RtMidiHostBackend::kLoopbackInputIndex, 128,
					in_handle) == MidiError::OK && in_handle != MidiBackend::NO_HANDLE,
			"open loopback input");
	check(backend.open_output(RtMidiHostBackend::kLoopbackOutputIndex, 128,
					out_handle) == MidiError::OK && out_handle != MidiBackend::NO_HANDLE,
			"open loopback output");

	// A short message: note_on ch=0 pitch=60 vel=100 (full-form 3 bytes).
	const uint8_t note[3] = { 0x90, 0x3c, 0x64 };
	check(backend.write(out_handle, note, 3) == MidiError::OK, "write note");
	Collector col;
	backend.poll_input(in_handle,
			[&col](uint32_t w, int) { col.push(w); });
	check(col.words.size() == 1, "note delivered as one word");
	if (col.words.size() == 1) {
		const uint32_t w = col.words[0];
		check((w & 0xff) == 0x90 && ((w >> 8) & 0xff) == 0x3c &&
						((w >> 16) & 0xff) == 0x64,
				"note word layout (low byte first)");
	}

	// A sysex: F0 00 01 02 03 04 F7 (7 bytes -> words {b0..b3},{b4..b6}).
	const uint8_t syx[7] = { 0xF0, 0x00, 0x01, 0x02, 0x03, 0x04, 0xF7 };
	check(backend.write_sysex(out_handle, syx, 7) == MidiError::OK,
			"write_sysex full F0..F7");
	col = Collector();
	backend.poll_input(in_handle,
			[&col](uint32_t w, int) { col.push(w); });
	check(col.words.size() == 2, "sysex delivered as two words");
	if (col.words.size() == 2) {
		check((col.words[0] & 0xff) == 0xF0 &&
						(((col.words[0] >> 8) & 0xff)) == 0x00 &&
						(((col.words[0] >> 16) & 0xff)) == 0x01 &&
						(((col.words[0] >> 24) & 0xff)) == 0x02,
				"sysex word 0 layout");
		check((col.words[1] & 0xff) == 0x03 &&
						(((col.words[1] >> 8) & 0xff)) == 0x04 &&
						(((col.words[1] >> 16) & 0xff)) == 0xF7,
				"sysex word 1 layout (F7 ends the message)");
	}

	// --- write_sysex shape guards -----------------------------------------
	const uint8_t bad[2] = { 0xF0, 0x01 }; // no F7 terminator
	check(backend.write_sysex(out_handle, bad, 2) == MidiError::InvalidParameter,
			"write_sysex rejects a payload without F7");
	const uint8_t notsyx[3] = { 0x90, 0x3c, 0x64 };
	check(backend.write_sysex(out_handle, notsyx, 3) == MidiError::InvalidParameter,
			"write_sysex rejects a non-sysex payload");

	// --- close / shutdown --------------------------------------------------
	check(backend.close(in_handle) == MidiError::OK, "close loopback input");
	check(backend.close(out_handle) == MidiError::OK, "close loopback output");
	// Closing the handles does NOT deactivate the facility (same
	// contract as the Android backend): the loopback stays listed and
	// a second create reports "already active" until shutdown().
	check(backend.create_virtual_loopback("m3-test-lb-2") == MidiError::Failed,
			"loopback re-create fails while active");
	backend.shutdown();
	check(!backend.available(), "unavailable after shutdown");
	check(backend.list_ports().empty(), "empty listing after shutdown");

	if (g_failures == 0) {
		std::printf("HOST_BACKEND_PASS\n");
		return 0;
	}
	std::printf("HOST_BACKEND_FAIL (%d)\n", g_failures);
	return 1;
}

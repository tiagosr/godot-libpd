// midi_writer_tests.cpp — pure C++ tests for MidiOutWriter (Task 4).
//
// MidiOutWriter is the running-status writer for raw [midiout] byte
// streams. It is pure (no PortMIDI, no threads, no I/O), so this test
// builds without portmidi.h and without linking any router code:
// the writer is implemented inline in src/midi_router.h.
//
// Pinned behavior (task-4-brief.md):
//   - a status byte (0x80..0xFF) is emitted only when it differs from the
//     previously stored status (running status);
//   - data bytes (0x00..0x7F) are always emitted;
//   - a F0..F7 sysex sequence is dropped whole (input-only sysex, spec §1);
//   - a non-F0/F7 status byte terminates a truncated sysex and is then
//     treated as an ordinary status byte;
//   - realtime bytes (F8..FF) are always emitted: they are complete
//     one-byte messages, never subject to running-status suppression;
//   - feed() returns the 0 or 1 bytes emitted for that one input byte.

#include "midi_router.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                            \
	{                                                                          \
		if (!(cond)) {                                                         \
			++failures;                                                        \
			std::printf("CHECK failed: %s (line %d)\n", #cond, __LINE__);      \
		}                                                                      \
	}

// feed p_byte and assert the per-call return equals p_expected_bytes
// (empty when the byte is suppressed/swallowed).
static void expect_feed(MidiOutWriter &p_w, uint8_t p_byte,
		const std::vector<uint8_t> &p_expected) {
	auto out = p_w.feed(p_byte);
	bool ok = out.size() == p_expected.size();
	if (ok) {
		for (size_t i = 0; i < out.size(); ++i) {
			if (out[i] != p_expected[i]) {
				ok = false;
			}
		}
	}
	if (!ok) {
		++failures;
		std::printf("writer mismatch (line %d): byte 0x%02X ->", __LINE__,
				p_byte);
		for (uint8_t b : out) {
			std::printf(" %02X", b);
		}
		std::printf(" (expected:");
		for (uint8_t b : p_expected) {
			std::printf(" %02X", b);
		}
		std::printf(")\n");
	}
}

// ---------------------------------------------------------------------------
// Pinned brief cases: 0x90 0x3C 0x64 / running 0x3D 0x50 / 0x91 0x00 0x40
// ---------------------------------------------------------------------------

static void test_pinned_running_status() {
	MidiOutWriter w;
	// Fresh writer, note on 0x90 C=0 pitch=60 vel=100.
	expect_feed(w, 0x90, {0x90});
	expect_feed(w, 0x3C, {0x3C});
	expect_feed(w, 0x64, {0x64});

	// Same-status continuation (running status): no status byte on the wire.
	expect_feed(w, 0x3D, {0x3D});
	expect_feed(w, 0x50, {0x50});

	// Status change 0x91: the new status is emitted, then its data bytes.
	expect_feed(w, 0x91, {0x91});
	expect_feed(w, 0x00, {0x00});
	expect_feed(w, 0x40, {0x40});
}

// ---------------------------------------------------------------------------
// Repeated identical status is suppressed, even mid-stream.
// ---------------------------------------------------------------------------

static void test_status_repeat_suppressed() {
	MidiOutWriter w;
	expect_feed(w, 0xB0, {0xB0}); // CC C=0
	expect_feed(w, 0x7B, {0x7B});
	expect_feed(w, 0x7F, {0x7F});
	expect_feed(w, 0xB0, {});     // same status: running, suppressed
	expect_feed(w, 0x07, {0x07}); // data still emitted
}

// ---------------------------------------------------------------------------
// Sysex (F0..F7) is dropped whole; stream resumes normally after EOX.
// ---------------------------------------------------------------------------

static void test_sysex_dropped() {
	MidiOutWriter w;
	expect_feed(w, 0x90, {0x90});
	expect_feed(w, 0x3C, {0x3C});
	expect_feed(w, 0x64, {0x64});

	expect_feed(w, 0xF0, {}); // sysex start: swallowed
	expect_feed(w, 0x79, {});
	expect_feed(w, 0x01, {});
	expect_feed(w, 0xF7, {}); // EOX: swallowed

	// The channel stream is unaffected by the dropped sysex.
	expect_feed(w, 0x90, {}); // still the same running status -> suppressed
	expect_feed(w, 0x3D, {0x3D});
}

// ---------------------------------------------------------------------------
// Truncated sysex: a non-F0/F7 status byte ends the (dropped) sysex and is
// processed as an ordinary status.
// ---------------------------------------------------------------------------

static void test_truncated_sysex() {
	MidiOutWriter w;
	expect_feed(w, 0x90, {0x90});
	expect_feed(w, 0x3C, {0x3C});
	expect_feed(w, 0x64, {0x64});

	expect_feed(w, 0xF0, {});
	expect_feed(w, 0x79, {}); // no F7 follows: truncated
	expect_feed(w, 0x90, {}); // terminates the sysex; same status -> running

	expect_feed(w, 0xF0, {});
	expect_feed(w, 0x79, {});
	expect_feed(w, 0x91, {0x91}); // different status -> emitted
}

// ---------------------------------------------------------------------------
// Realtime bytes (F8..FF) are complete one-byte messages on the MIDI wire
// and are NEVER subject to running-status suppression: a repeated F8 is
// (e.g.) a MIDI clock tick, and every tick must go out.
// ---------------------------------------------------------------------------

static void test_realtime_always_emitted() {
	MidiOutWriter w;
	expect_feed(w, 0xF8, {0xF8}); // clock tick
	expect_feed(w, 0xF8, {0xF8}); // repeated: still a full message
	expect_feed(w, 0xF8, {0xF8}); // every tick emits
	expect_feed(w, 0xF9, {0xF9}); // different status: emitted
	expect_feed(w, 0xF8, {0xF8}); // back to clock: emitted again

	// Realtime does not disturb channel running-status memory.
	MidiOutWriter c;
	expect_feed(c, 0x90, {0x90});
	expect_feed(c, 0x3C, {0x3C});
	expect_feed(c, 0x64, {0x64});
	expect_feed(c, 0xF8, {0xF8}); // clock tick in the middle
	expect_feed(c, 0x90, {});     // channel status still running-suppressed
	expect_feed(c, 0x3D, {0x3D});
}

// ---------------------------------------------------------------------------
// A data byte before any status byte is emitted by the writer (the router's
// output framing drops it there: no status anchor means no message).
// ---------------------------------------------------------------------------

static void test_stray_data_byte() {
	MidiOutWriter w;
	expect_feed(w, 0x3C, {0x3C}); // stray data: writer emits it
	expect_feed(w, 0x90, {0x90});
	expect_feed(w, 0x3C, {0x3C});
	expect_feed(w, 0x64, {0x64});
}

int main() {
	test_pinned_running_status();
	test_status_repeat_suppressed();
	test_sysex_dropped();
	test_truncated_sysex();
	test_realtime_always_emitted();
	test_stray_data_byte();

	if (failures > 0) {
		std::printf("%d MIDI WRITER CHECKS FAILED\n", failures);
		return 1;
	}
	std::printf("ALL MIDI WRITER TESTS PASSED\n");
	return 0;
}

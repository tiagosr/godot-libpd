// v2 M2 Task 2 — RtMidi Android backend: platform-free seam tests.
//
// The RtMidi/AMIDI glue only compiles on __ANDROID__, but the
// pure logic it depends on (message -> word chop, unified ->
// filtered device position, the word ring with its overflow
// reporting) lives unguarded in midi_backend_rtmidi.h. Testing it
// on every host pins the exact byte/word semantics the Android
// backend will run on device (Task 3/4 verifies the glue itself).

#include "midi_backend_rtmidi.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

using godot_libpd::MidiBackend;
using godot_libpd::rtmidi_seam::chop_to_words;
using godot_libpd::rtmidi_seam::DeviceSide;
using godot_libpd::rtmidi_seam::filtered_position;
using godot_libpd::rtmidi_seam::WordRing;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(expr)                                                        \
	do {                                                                   \
		++g_checks;                                                        \
		if (!(expr)) {                                                     \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
		}                                                                  \
	} while (0)

static std::vector<uint32_t> collect(const uint8_t *bytes, int len) {
	std::vector<uint32_t> words;
	chop_to_words(bytes, len, [&](uint32_t w, int) { words.push_back(w); });
	return words;
}

// Same as collect() but keeps each word's valid-byte count (what the
// read stage relies on to avoid truncating data-first continuation
// words — the running-status regression).
static std::vector<std::pair<uint32_t, int>> collect_counts(const uint8_t *bytes,
		int len) {
	std::vector<std::pair<uint32_t, int>> out;
	chop_to_words(bytes, len, [&](uint32_t w, int c) { out.push_back({w, c}); });
	return out;
}

static void test_chop() {
	// Short message: 3 bytes -> one word, low byte first.
	{
		const uint8_t note_on[3] = {0x90, 0x45, 0x7F};
		auto w = collect(note_on, 3);
		CHECK(w.size() == 1);
		if (w.size() == 1) {
			CHECK(w[0] == 0x7F4590);
		}
	}
	// 4-byte message (e.g. packed PmEvent layout): one full word.
	{
		const uint8_t four[4] = {0x01, 0x02, 0x03, 0x04};
		auto w = collect(four, 4);
		CHECK(w.size() == 1);
		if (w.size() == 1) {
			CHECK(w[0] == 0x04030201);
		}
	}
	// 5-byte message: 4 + 1 (the read stage reassembles stream
	// bytes; it only needs <= 4 per word).
	{
		const uint8_t five[5] = {0xF0, 0x7E, 0x7F, 0x09, 0xF7};
		auto w = collect(five, 5);
		CHECK(w.size() == 2);
		if (w.size() == 2) {
			CHECK(w[0] == 0x097F7EF0);
			CHECK(w[1] == 0xF7);
		}
	}
	// Full 257-byte MIDI-1 sysex (F0 + 255 data + F7): 65 words
	// (64 full + 1 with the trailing F7).
	{
		std::vector<uint8_t> msg(257, 0x42);
		msg.front() = 0xF0;
		msg.back() = 0xF7;
		auto w = collect(msg.data(), 257);
		CHECK(w.size() == 65);
		if (w.size() == 65) {
			CHECK(w[0] == 0x424242F0);
			CHECK(w[64] == 0xF7);
		}
	}
	// Degenerate lengths.
	{
		CHECK(collect(nullptr, 0).empty());
		const uint8_t one[1] = {0xF8};
		auto w = collect(one, 1);
		CHECK(w.size() == 1);
		if (w.size() == 1) {
			CHECK(w[0] == 0xF8);
		}
	}
}

// ---------------------------------------------------------------------------
// chop_to_words valid-byte counts: the read stage relies on these to
// avoid truncating a data-first continuation word (running status).
// ---------------------------------------------------------------------------

static void test_chop_counts() {
	// Running-status pair: 0x90 0x3C 0x64 | 0x3D 0x50. The 5th wire byte
	// (0x50) lands in the tail of a data-first word; the counts MUST be
	// 4 and 1, otherwise the read stage truncates 0x50 and the second
	// note is broken.
	const uint8_t pair[5] = {0x90, 0x3C, 0x64, 0x3D, 0x50};
	auto wc = collect_counts(pair, 5);
	CHECK(wc.size() == 2);
	if (wc.size() == 2) {
		CHECK(wc[0].first == 0x3D643C90);
		CHECK(wc[0].second == 4);
		CHECK(wc[1].first == 0x50);
		CHECK(wc[1].second == 1);
	}
	// A 3-byte note-on is one word with count 3 (no padding).
	const uint8_t note_on[3] = {0x90, 0x45, 0x7F};
	auto w = collect_counts(note_on, 3);
	CHECK(w.size() == 1);
	if (w.size() == 1) {
		CHECK(w[0].second == 3);
	}
	// A full 4-byte word has count 4.
	const uint8_t four[4] = {0x01, 0x02, 0x03, 0x04};
	auto f = collect_counts(four, 4);
	CHECK(f.size() == 1);
	if (f.size() == 1) {
		CHECK(f[0].second == 4);
	}
}

static void test_filtered_position() {
	// Raw order: 0 = in+out, 1 = input only, 2 = output only,
	// 3 = in+out.
	std::vector<DeviceSide> devices;
	{
		DeviceSide d;
		d.input = true;
		d.output = true;
		devices.push_back(d);
	}
	{
		DeviceSide d;
		d.input = true;
		devices.push_back(d);
	}
	{
		DeviceSide d;
		d.output = true;
		devices.push_back(d);
	}
	{
		DeviceSide d;
		d.input = true;
		d.output = true;
		devices.push_back(d);
	}
	// Input-filtered list: [dev0, dev1, dev3] -> positions 0,1,2.
	CHECK(filtered_position(devices, 0, true) == 0);
	CHECK(filtered_position(devices, 1, true) == 1);
	CHECK(filtered_position(devices, 2, true) == -1); // no input side
	CHECK(filtered_position(devices, 3, true) == 2);
	// Output-filtered list: [dev0, dev2, dev3] -> positions 0,1,2.
	CHECK(filtered_position(devices, 0, false) == 0);
	CHECK(filtered_position(devices, 1, false) == -1); // no output side
	CHECK(filtered_position(devices, 2, false) == 1);
	CHECK(filtered_position(devices, 3, false) == 2);
	// Bounds.
	CHECK(filtered_position(devices, -1, true) == -1);
	CHECK(filtered_position(devices, 4, true) == -1);
	CHECK(filtered_position({}, 0, true) == -1);
}

static void test_word_ring() {
	// Basic push/drain order.
	{
		WordRing ring(8);
		ring.push(0xAA, 1);
		ring.push(0xBB, 1);
		std::vector<uint32_t> got;
		CHECK(!ring.drain([&](uint32_t w, int) { got.push_back(w); }));
		CHECK(got.size() == 2);
		if (got.size() == 2) {
			CHECK(got[0] == 0xAA);
			CHECK(got[1] == 0xBB);
		}
		// Second drain: empty, no overflow.
		got.clear();
		CHECK(!ring.drain([&](uint32_t w, int) { got.push_back(w); }));
		CHECK(got.empty());
	}
	// Overflow is reported on the first drain after a drop, then the
	// flag clears (sticky-until-reported — the router resets the
	// port's read state on the report, the port stays open).
	{
		WordRing ring(2);
		ring.push(1, 1);
		ring.push(2, 1);
		ring.push(3, 1); // dropped (cap 2) — overflow flag set
		std::vector<uint32_t> got;
		CHECK(ring.drain([&](uint32_t w, int) { got.push_back(w); })); // overflow
		CHECK(got.size() == 2);
		if (got.size() == 2) {
			CHECK(got[0] == 1);
			CHECK(got[1] == 2);
		}
		CHECK(!ring.drain([](uint32_t, int) {})); // flag cleared after report
		ring.push(4, 1);
		ring.push(5, 1);
		ring.push(6, 1); // dropped
		got.clear();
		CHECK(ring.drain([&](uint32_t w, int) { got.push_back(w); })); // overflow
		CHECK(got.size() == 2);
		if (got.size() == 2) {
			CHECK(got[0] == 4);
			CHECK(got[1] == 5);
		}
		CHECK(!ring.drain([](uint32_t, int) {}));
	}
	// Reset clears pending words and the flag.
	{
		WordRing ring(2);
		ring.push(1, 1);
		ring.push(2, 1);
		ring.push(3, 1); // overflow
		ring.reset();
		std::vector<uint32_t> got;
		CHECK(!ring.drain([&](uint32_t w, int) { got.push_back(w); }));
		CHECK(got.empty());
	}
	// Producer/consumer under concurrent threads (the RtMidi
	// callback thread vs the router I/O thread): words below the
	// cap are never lost; over the cap, drops happen with >= 1
	// overflow report and no crash.
	{
		WordRing ring(64);
		const int kTotal = 5000;
		std::atomic<int> count{0};
		std::atomic<bool> producer_done{false};
		auto producer = std::thread([&] {
			for (int i = 0; i < kTotal; ++i) {
				ring.push(static_cast<uint32_t>(i), 1);
			}
			producer_done = true;
		});
		int overflows = 0;
		bool drained_all = false;
		while (!drained_all) {
			// drain() returns true while the overflow flag is set;
			// after a drain the ring is empty, so (producer done AND
			// this drain empty) means everything that will come has
			// been taken.
			const bool more =
					ring.drain([&](uint32_t, int) { ++count; });
			if (more) {
				++overflows;
			}
			drained_all = producer_done.load() && !more;
			std::this_thread::yield();
		}
		producer.join();
		CHECK(overflows >= 1); // 5000 words through a 64 cap: certain
		CHECK(count > 0);
		CHECK(count < kTotal);
	}
}

int main() {
	test_chop();
	test_chop_counts();
	test_filtered_position();
	test_word_ring();
	if (g_failures == 0) {
		std::printf("midi_rtmidi_seam_tests OK (%d checks)\n", g_checks);
		return 0;
	}
	std::printf("%d FAILURE(S) in %d checks\n", g_failures, g_checks);
	return 1;
}


// midi_read_stage_tests.cpp — pure C++ tests for MidiReadStage, the
// per-port sysex read stage (Task 4 rework).
//
// The vendored PortMIDI 2.0.7 delivers PmEvent.message as a plain
// "up to 4 bytes, low byte first" word — NO 0xFF000000 long-event flag
// (the backend never sets it): short events start at byte 0; sysex
// arrives as an F0-first run of words terminated by an F7 that
// zero-pads the rest of its word (the backend enqueues the word at F7
// and resets). MidiReadStage re-assembles the words into raw stream
// bytes; the framer re-frames the stream.
//
// MidiReadStage is pure (no PortMIDI, no threads, fixed state) and lives
// inline in src/midi_router.h, so this test builds without portmidi.h;
// it links the framer only for the end-to-end cases.
//
// Pinned behavior under test:
//   - short events: status + 0-2 data bytes from byte 0, byte-identical
//     to the pre-rework read stage EXCEPT F4..F7, which are single-byte
//     system-common messages (vendored pm_midi_length semantics);
//   - F0 in byte 0 opens the per-port sysex and emits the word's sysex
//     bytes; continuation words emit their bytes up to and including
//     the F7 that terminates the message;
//   - zero bytes after the F7 in the same word are padding (never
//     emitted); a non-zero byte after the F7 starts the next event:
//     status (>=0x80) -> short event (as many bytes as fit in the
//     word), data (<0x80) -> one running-status continuation byte;
//   - the stream feeds MidiFramer unchanged: the framer's on_sysex body
//     is F0-exclusive / F7-inclusive, so the emitted `midi_sysex`
//     signal (the server prepends the F0) is exactly one full F0..F7
//     message.

#include "midi_router.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                       \
	{                                                                     \
		if (!(cond)) {                                                    \
			++failures;                                                   \
			std::printf("CHECK failed: %s (line %d)\n", #cond, __LINE__); \
		}                                                                 \
	}

// Pack up to 4 bytes low-byte-first into a PmEvent word (zero padding).
static uint32_t word(std::initializer_list<uint8_t> p_bytes) {
	uint32_t w = 0;
	int i = 0;
	for (uint8_t b : p_bytes) {
		w |= static_cast<uint32_t>(b) << (8 * i);
		++i;
	}
	return w;
}

// Feed one word with an explicit valid-byte count; return the stream.
static std::vector<uint8_t> feed_word(MidiReadStage &p_stage, uint32_t p_word,
		int p_count) {
	std::vector<uint8_t> out;
	p_stage.feed(p_word, p_count, [&out](uint8_t b) { out.push_back(b); });
	return out;
}

// Feed a list of (word, valid-byte count) pairs; return the accumulated
// stream. The count is the number of real bytes in the word (short
// events carry midi_short_bytes(status); sysex words are full 4-byte
// chunks; running-status data words carry their data bytes).
static std::vector<uint8_t> feed_words(MidiReadStage &p_stage,
		std::initializer_list<std::pair<uint32_t, int>> p_words) {
	std::vector<uint8_t> out;
	for (const auto &wc : p_words) {
		p_stage.feed(wc.first, wc.second, [&out](uint8_t b) { out.push_back(b); });
	}
	return out;
}

static void expect_stream(const char *p_what, int p_line,
		const std::vector<uint8_t> &p_actual,
		std::initializer_list<uint8_t> p_expected) {
	if (p_actual == std::vector<uint8_t>(p_expected)) {
		return;
	}
	++failures;
	std::printf("stream mismatch %s (line %d): got", p_what, p_line);
	for (uint8_t b : p_actual) {
		std::printf(" %02X", b);
	}
	std::printf(" (expected:");
	for (uint8_t b : p_expected) {
		std::printf(" %02X", b);
	}
	std::printf(")\n");
}

// ---------------------------------------------------------------------------
// Short events: byte-identical to the old read stage, except F4..F7.
// ---------------------------------------------------------------------------

static void test_short_event_regression() {
	MidiReadStage s;
	expect_stream("note on", __LINE__,
			feed_word(s, word({0x90, 0x3C, 0x64}), 3), {0x90, 0x3C, 0x64});
	expect_stream("note off", __LINE__,
			feed_word(s, word({0x80, 0x3C, 0x40}), 3), {0x80, 0x3C, 0x40});
	expect_stream("cc", __LINE__,
			feed_word(s, word({0xB0, 0x7B, 0x07}), 3), {0xB0, 0x7B, 0x07});
	expect_stream("poly", __LINE__,
			feed_word(s, word({0xA0, 0x3C, 0x20}), 3), {0xA0, 0x3C, 0x20});
	expect_stream("pitch bend", __LINE__,
			feed_word(s, word({0xE0, 0x00, 0x40}), 3), {0xE0, 0x00, 0x40});
	// 2-byte events: the zero padding must NOT be emitted.
	expect_stream("program change", __LINE__,
			feed_word(s, word({0xC0, 0x40, 0x00, 0x00}), 2), {0xC0, 0x40});
	expect_stream("channel aftertouch", __LINE__,
			feed_word(s, word({0xD0, 0x20, 0x00, 0x00}), 2), {0xD0, 0x20});
	// System common: MTC quarter frame (2), song position (3), song
	// select (2).
	expect_stream("mtc quarter frame", __LINE__,
			feed_word(s, word({0xF1, 0x78, 0x00, 0x00}), 2), {0xF1, 0x78});
	expect_stream("song position", __LINE__,
			feed_word(s, word({0xF2, 0x00, 0x40}), 3), {0xF2, 0x00, 0x40});
	expect_stream("song select", __LINE__,
			feed_word(s, word({0xF3, 0x00, 0x00, 0x00}), 2), {0xF3, 0x00});
	// Realtime: always one byte.
	expect_stream("clock", __LINE__, feed_word(s, word({0xF8, 0x00, 0x00, 0x00}), 1), {0xF8});
	expect_stream("active sense", __LINE__, feed_word(s, word({0xFE}), 1), {0xFE});
	expect_stream("reset", __LINE__, feed_word(s, word({0xFF}), 1), {0xFF});
}

// ---------------------------------------------------------------------------
// F4..F7 are single-byte system-common messages (vendored
// pm_midi_length semantics): F4..F7 returned 2 before the fix.
// ---------------------------------------------------------------------------

static void test_f4_f7_single_byte() {
	MidiReadStage s;
	// Direct assertions on the byte count helper.
	CHECK(MidiReadStage::pm_short_bytes(0xF4) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xF5) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xF6) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xF7) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xF1) == 2);
	CHECK(MidiReadStage::pm_short_bytes(0xF2) == 3);
	CHECK(MidiReadStage::pm_short_bytes(0xF3) == 2);
	CHECK(MidiReadStage::pm_short_bytes(0xF8) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xF9) == 1);
	CHECK(MidiReadStage::pm_short_bytes(0xFF) == 1);

	// raw_data_need (output [midiout] path) must mirror pm_midi_length - 1
	// EXACTLY (round-trip consistency with the vendored input parser —
	// upstream counts F1 as 2 bytes and F2 as 3, not the spec values):
	CHECK(MidiReadStage::raw_data_need(0x90) == 2);
	CHECK(MidiReadStage::raw_data_need(0xB0) == 2);
	CHECK(MidiReadStage::raw_data_need(0xC0) == 1);
	CHECK(MidiReadStage::raw_data_need(0xD0) == 1);
	CHECK(MidiReadStage::raw_data_need(0xE0) == 2);
	CHECK(MidiReadStage::raw_data_need(0xF1) == 1);
	CHECK(MidiReadStage::raw_data_need(0xF2) == 2);
	CHECK(MidiReadStage::raw_data_need(0xF3) == 1);
	CHECK(MidiReadStage::raw_data_need(0xF4) == 0);
	CHECK(MidiReadStage::raw_data_need(0xF5) == 0);
	CHECK(MidiReadStage::raw_data_need(0xF6) == 0);
	CHECK(MidiReadStage::raw_data_need(0xF7) == 0);
	CHECK(MidiReadStage::raw_data_need(0xF8) == 0);
	CHECK(MidiReadStage::raw_data_need(0xFF) == 0);
	CHECK(MidiReadStage::pm_short_bytes(0xC0) == 2);
	CHECK(MidiReadStage::pm_short_bytes(0xD0) == 2);
	CHECK(MidiReadStage::pm_short_bytes(0x90) == 3);
	CHECK(MidiReadStage::pm_short_bytes(0xE0) == 3);
	CHECK(MidiReadStage::pm_short_bytes(0x00) == 1); // safe default
	// Through the stage: exactly one byte out per word.
	expect_stream("tune request", __LINE__, feed_word(s, word({0xF4}), 1), {0xF4});
	expect_stream("end of cable", __LINE__, feed_word(s, word({0xF5}), 1), {0xF5});
	expect_stream("rt reset", __LINE__, feed_word(s, word({0xF6}), 1), {0xF6});
	// A bare F7 outside sysex is a realtime-adjacent system-common
	// one-byte message, not a sysex terminator: emitted once.
	expect_stream("stray f7", __LINE__, feed_word(s, word({0xF7}), 1), {0xF7});
	CHECK(!s.in_sysex);
}

// ---------------------------------------------------------------------------
// Multi-word sysex: F0-first words, F7 mid-word, zero padding dropped.
// ---------------------------------------------------------------------------

static void test_multi_word_sysex() {
	MidiReadStage s;
	CHECK(!s.in_sysex);
	std::vector<uint8_t> stream = feed_words(s, {
		{word({0xF0, 0x01, 0x02, 0x03}), 4},
		{word({0x04, 0x05, 0x06, 0x07}), 4},
		{word({0x08, 0x09, 0xF7, 0x00}), 4},
	});
	CHECK(!s.in_sysex); // completed
	expect_stream("multi word", __LINE__, stream,
			{0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xF7});
}

static void test_f7_at_word_positions() {
	// F7 as the LAST byte of a word: nothing follows it.
	{
		MidiReadStage s;
		expect_stream("f7 last", __LINE__,
				feed_words(s, {{word({0xF0, 0x01, 0x02, 0x03}), 4}, {word({0x04, 0x05, 0x06, 0xF7}), 4}}),
				{0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xF7});
		CHECK(!s.in_sysex);
	}
	// F7 as the FIRST byte of a word: the continuation words before it
	// plus just the F7.
	{
		MidiReadStage s;
		expect_stream("f7 first", __LINE__,
				feed_words(s, {{word({0xF0, 0x01, 0x02, 0x03}), 4}, {word({0xF7, 0x00, 0x00, 0x00}), 4}}),
				{0xF0, 0x01, 0x02, 0x03, 0xF7});
	}
	// F7 in the middle (byte 2) of a word: stops after it; byte 3 is
	// padding and must not be emitted.
	{
		MidiReadStage s;
		expect_stream("f7 middle", __LINE__,
				feed_words(s, {{word({0xF0, 0x01, 0xF7, 0x00}), 4}}),
				{0xF0, 0x01, 0xF7});
	}
	// Zero-length sysex (F0 F7) in one word.
	{
		MidiReadStage s;
		expect_stream("zero length", __LINE__,
				feed_words(s, {{word({0xF0, 0xF7, 0x00, 0x00}), 4}}),
				{0xF0, 0xF7});
	}
}

// ---------------------------------------------------------------------------
// F7 mid-word + a non-zero byte after it: the byte starts the next
// event (status -> short event; data -> one running-status byte).
// ---------------------------------------------------------------------------

static void test_f7_then_next_event() {
	// F7 mid-word + status byte: the short event starts at byte k+1,
	// but only the bytes present in this word are emitted; the rest
	// arrives as later words.
	MidiReadStage s;
	expect_stream("f7 then status", __LINE__,
			feed_words(s, {{word({0xF0, 0x01, 0xF7, 0x90}), 4}}),
			{0xF0, 0x01, 0xF7, 0x90});
	expect_stream("note data", __LINE__,
			feed_words(s, {{word({0x3C}), 1}, {word({0x64}), 1}}),
			{0x3C, 0x64});

	// F7 first byte + a complete 2-byte short event (program change) in
	// the same word.
	MidiReadStage s2;
	expect_stream("f7 then program change", __LINE__,
			feed_words(s2, {{word({0xF0, 0xF7, 0xC0, 0x40}), 4}}),
			{0xF0, 0xF7, 0xC0, 0x40});

	// F7 mid-word + data byte (< 0x80): exactly one running-status
	// continuation byte is emitted; the later bytes of the word stay
	// padding.
	MidiReadStage s3;
	expect_stream("f7 then running data", __LINE__,
			feed_words(s3, {{word({0xF0, 0x01, 0xF7, 0x3C}), 4}}),
			{0xF0, 0x01, 0xF7, 0x3C});
}

// ---------------------------------------------------------------------------
// End-to-end with the framer: sysex then short messages.
// ---------------------------------------------------------------------------

struct TestSink : MidiFramingSink {
	std::vector<uint8_t> sysex_body;
	int sysex_count = 0;
	int truncated_count = 0;
	std::vector<MidiShortMsg> shorts;
	std::vector<uint8_t> bytes;
	void on_short(const MidiShortMsg &p_msg) override { shorts.push_back(p_msg); }
	void on_byte(uint8_t p_byte) override { bytes.push_back(p_byte); }
	void on_sysex(const uint8_t *p_data, int p_len) override {
		++sysex_count;
		sysex_body.assign(p_data, p_data + p_len);
	}
	void on_sysex_truncated() override {
		++truncated_count;
	}
};

static void test_sysex_then_notes_end_to_end() {
	TestSink sink;
	MidiFramer framer(sink);
	MidiReadStage s;
	// Sysex F0 01 02 03 04 05 F7 (F7 mid-word, rest padding), then
	// note on 60/100 and note off on channel 0.
	for (const auto &wc : {std::pair<uint32_t, int>{word({0xF0, 0x01, 0x02, 0x03}), 4},
			std::pair<uint32_t, int>{word({0x04, 0x05, 0xF7, 0x00}), 4},
			std::pair<uint32_t, int>{word({0x90, 0x3C, 0x64}), 3},
			std::pair<uint32_t, int>{word({0x80, 0x3C, 0x00}), 3}}) {
		s.feed(wc.first, wc.second, [&framer](uint8_t b) { framer.feed(b); });
	}
	CHECK(sink.sysex_count == 1);
	CHECK(sink.truncated_count == 0);
	CHECK(sink.sysex_body == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04, 0x05, 0xF7}));
	CHECK(sink.shorts.size() == 2);
	if (sink.shorts.size() == 2) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 0x3C);
		CHECK(sink.shorts[0].d2 == 0x64);
		CHECK(sink.shorts[1].kind == MidiKind::NOTE_OFF);
		CHECK(sink.shorts[1].d1 == 0x3C);
	}
	// Pinned signal contract: the framer body is F0-exclusive / F7-
	// inclusive; the server prepends the F0, so the emitted `midi_sysex`
	// data is exactly one full F0..F7 message.
	std::vector<uint8_t> signal_data;
	signal_data.push_back(0xF0);
	signal_data.insert(signal_data.end(), sink.sysex_body.begin(),
			sink.sysex_body.end());
	expect_stream("signal contract", __LINE__, signal_data,
			{0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7});
}

// ---------------------------------------------------------------------------
// State hygiene: pending sysex across words, reset, completion.
// ---------------------------------------------------------------------------

static void test_sysex_state_and_reset() {
	MidiReadStage s;
	feed_word(s, word({0xF0, 0x01, 0x02, 0x03}), 4);
	CHECK(s.in_sysex); // pending after the first word
	// Port close / buffer-overflow reset drops the pending state; the
	// next word is treated as a fresh short event.
	s.reset();
	CHECK(!s.in_sysex);
	expect_stream("after reset", __LINE__,
			feed_word(s, word({0x90, 0x3C, 0x64}), 3), {0x90, 0x3C, 0x64});

	// A completed sysex leaves the state clear.
	MidiReadStage s2;
	feed_word(s2, word({0xF0, 0xF7}), 2);
	CHECK(!s2.in_sysex);
}

// ---------------------------------------------------------------------------
// Running status preserved across the 4-byte word boundary: a
// running-status pair (a status byte + a second data-only event) is
// chopped by the backend into two words, with the 5th wire byte landing
// in the tail of the second (data-first) word. Before the count-based
// read stage, that byte was truncated (a data-first word was sized to a
// single byte), silently breaking the second note.
// ---------------------------------------------------------------------------

static void test_running_status_split_across_words() {
	// Wire stream (running status): 0x90 0x3C 0x64 | 0x3D 0x50
	// (note-on 60/100, then note-on 61/80 with the status implied).
	// Chopped to 4-byte words: {90,3C,64,3D} count=4, then {50} count=1.
	MidiReadStage s;
	std::vector<uint8_t> stream = feed_words(s, {
		{word({0x90, 0x3C, 0x64, 0x3D}), 4},
		{word({0x50}), 1},
	});
	// The pristine running-status stream is preserved: b4 (0x50) is NOT
	// dropped -- the stream is 0x90 0x3C 0x64 0x3D 0x50.
	expect_stream("running status preserved", __LINE__, stream,
			{0x90, 0x3C, 0x64, 0x3D, 0x50});

	// ...and the framer decodes it to two note-ons (the second with the
	// running status applied). Wire the read stage straight into the
	// framer (feed_words would accumulate into a throwaway vector).
	TestSink sink;
	MidiFramer framer(sink);
	MidiReadStage s2;
	for (const auto &wc : {std::pair<uint32_t, int>{word({0x90, 0x3C, 0x64, 0x3D}), 4},
			std::pair<uint32_t, int>{word({0x50}), 1}}) {
		s2.feed(wc.first, wc.second, [&framer](uint8_t b) { framer.feed(b); });
	}
	CHECK(sink.shorts.size() == 2);
	if (sink.shorts.size() == 2) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].d1 == 0x3C);
		CHECK(sink.shorts[0].d2 == 0x64);
		CHECK(sink.shorts[1].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[1].d1 == 0x3D);
		CHECK(sink.shorts[1].d2 == 0x50);
	}
}

int main() {
	test_short_event_regression();
	test_f4_f7_single_byte();
	test_multi_word_sysex();
	test_f7_at_word_positions();
	test_f7_then_next_event();
	test_sysex_then_notes_end_to_end();
	test_sysex_state_and_reset();
	test_running_status_split_across_words();

	if (failures > 0) {
		std::printf("%d MIDI READ STAGE CHECKS FAILED\n", failures);
		return 1;
	}
	std::printf("ALL MIDI READ STAGE TESTS PASSED\n");
	return 0;
}

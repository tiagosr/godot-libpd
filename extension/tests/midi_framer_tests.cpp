// Unit tests for MidiFramer (Task 1): pure running-status parser.
// Plain assert-style harness: any failure prints and exits non-zero.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "core/pd_midi_framer.h"

using namespace godot_libpd;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                               \
	} while (0)

// Collects everything the framer emits, for assertion.
struct RecordingSink : MidiFramingSink {
	std::vector<uint8_t> bytes;
	std::vector<MidiShortMsg> shorts;
	std::vector<std::vector<uint8_t>> sysex;
	int sysex_truncated_calls = 0;

	void on_short(const MidiShortMsg &p_msg) override {
		shorts.push_back(p_msg);
	}
	void on_byte(uint8_t p_byte) override {
		bytes.push_back(p_byte);
	}
	void on_sysex(const uint8_t *p_data, int p_len) override {
		sysex.emplace_back(p_data, p_data + p_len);
	}
	void on_sysex_truncated() override {
		sysex_truncated_calls++;
	}
};

static void feed_all(MidiFramer &p_framer, std::initializer_list<uint8_t> p_bytes) {
	for (uint8_t b : p_bytes) {
		p_framer.feed(b);
	}
}

static void test_note_on_explicit() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0x90, 0x3C, 0x64});
	CHECK(sink.bytes == (std::vector<uint8_t>{0x90, 0x3C, 0x64}));
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 100);
	}
	std::printf("note_on_explicit done\n");
}

static void test_note_off_and_vel0() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0x80, 0x3C, 0x40});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_OFF);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 64);
	}
	// Vel-0 note-on is reported as NOTE_OFF.
	feed_all(framer, {0x90, 0x3C, 0x00});
	CHECK(sink.shorts.size() == 2);
	if (sink.shorts.size() == 2) {
		CHECK(sink.shorts[1].kind == MidiKind::NOTE_OFF);
		CHECK(sink.shorts[1].channel == 0);
		CHECK(sink.shorts[1].d1 == 60);
		CHECK(sink.shorts[1].d2 == 0);
	}
	std::printf("note_off_and_vel0 done\n");
}

static void test_running_status() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0x90, 0x3C, 0x64, 0x3D, 0x50});
	CHECK(sink.bytes == (std::vector<uint8_t>{0x90, 0x3C, 0x64, 0x3D, 0x50}));
	CHECK(sink.shorts.size() == 2);
	if (sink.shorts.size() == 2) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 100);
		CHECK(sink.shorts[1].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[1].channel == 0);
		CHECK(sink.shorts[1].d1 == 61);
		CHECK(sink.shorts[1].d2 == 80);
	}
	std::printf("running_status done\n");
}

static void test_cc() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0xB3, 0x07, 0x7F});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::CC);
		CHECK(sink.shorts[0].channel == 3);
		CHECK(sink.shorts[0].d1 == 7);
		CHECK(sink.shorts[0].d2 == 127);
	}
	std::printf("cc done\n");
}

static void test_program_change_2byte() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0xC1, 0x2A});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::PROGRAM_CHANGE);
		CHECK(sink.shorts[0].channel == 1);
		CHECK(sink.shorts[0].d1 == 42);
	}
	std::printf("program_change_2byte done\n");
}

static void test_pitch_bend_14bit() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0xE0, 0x00, 0x40});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::PITCH_BEND);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 0); // low 7 bits
		CHECK(sink.shorts[0].d2 == 64); // high 7 bits
		// 14-bit value per standard assembly (d1 = low 7, d2 = high 7).
		// Note: the task brief annotates this case as "value 16384", which is
		// inconsistent with its own pinned d1/d2 (64 << 7 | 0 = 8192; 16384
		// would be a << 8 shift). Field pins are authoritative here.
		CHECK((int(sink.shorts[0].d1) | (int(sink.shorts[0].d2) << 7)) == 8192);
	}
	std::printf("pitch_bend_14bit done\n");
}

static void test_aftertouch_and_poly() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0xD0, 0x5A});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::AFTERTOUCH);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 90);
	}
	feed_all(framer, {0xA0, 0x3C, 0x64});
	CHECK(sink.shorts.size() == 2);
	if (sink.shorts.size() == 2) {
		CHECK(sink.shorts[1].kind == MidiKind::POLY_AFTERTOUCH);
		CHECK(sink.shorts[1].channel == 0);
		CHECK(sink.shorts[1].d1 == 60); // pitch
		CHECK(sink.shorts[1].d2 == 100); // value
	}
	std::printf("aftertouch_and_poly done\n");
}

static void test_sysex_whole() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0xF0, 0x7E, 0x7F, 0x09, 0xF7});
	CHECK(sink.bytes.empty()); // zero on_byte for all 5 sysex bytes
	CHECK(sink.shorts.empty());
	CHECK(sink.sysex.size() == 1);
	if (sink.sysex.size() == 1) {
		// Body between F0 and F7: exclusive of F0, inclusive of F7.
		CHECK(sink.sysex[0] == (std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0xF7}));
	}
	CHECK(sink.sysex_truncated_calls == 0);
	std::printf("sysex_whole done\n");
}

static void test_sysex_truncation() {
	RecordingSink sink;
	MidiFramer framer(sink);
	framer.feed(0xF0);
	for (int i = 0; i < 200; i++) {
		framer.feed(0x42);
	}
	framer.feed(0xF7);
	CHECK(sink.sysex.size() == 1);
	if (sink.sysex.size() == 1) {
		CHECK(sink.sysex[0].size() == 127);
		CHECK(sink.sysex[0][0] == 0x42);
		CHECK(sink.sysex[0][126] == 0x42);
	}
	CHECK(sink.sysex_truncated_calls == 1);
	// Framer is back to normal: next message parses.
	feed_all(framer, {0x90, 0x00, 0x40});
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 0);
		CHECK(sink.shorts[0].d2 == 64);
	}
	std::printf("sysex_truncation done\n");
}

static void test_realtime_interleaved() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0x90, 0x3C, 0xF1, 0x64});
	CHECK(sink.bytes == (std::vector<uint8_t>{0x90, 0x3C, 0xF1, 0x64}));
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 100);
	}
	std::printf("realtime_interleaved done\n");
}

static void test_garbage_no_status() {
	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {0x42, 0x43, 0x90, 0x3C, 0x64});
	// Data-only bytes before any status are dropped entirely (not emitted);
	// the first real status starts cleanly.
	CHECK(sink.bytes == (std::vector<uint8_t>{0x90, 0x3C, 0x64}));
	CHECK(sink.shorts.size() == 1);
	if (sink.shorts.size() == 1) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 100);
	}
	std::printf("garbage_no_status done\n");
}

static void test_byte_fidelity() {
	// 40-byte mixed stream: explicit + running + 1 sysex + 1 realtime.
	static const uint8_t stream[40] = {
			0x90, 0x3C, 0x64, // explicit note-on ch0 60/100
			0x3D, 0x50, // running note-on ch0 61/80
			0xB3, 0x07, 0x7F, // CC ch3 ctrl 7 val 127
			0xF0, 0x7E, 0x7F, 0x09, 0x06, 0x01, 0x01, 0xF7, // sysex (8 bytes)
			0xF2, // realtime
			0xC0, 0x2A, // program change ch0 42
			0xE0, 0x40, 0x00, // pitch bend ch0
			0xA2, 0x24, 0x32, // poly aftertouch ch2
			0xD1, 0x2A, // aftertouch ch1
			0x80, 0x2E, 0x00, // note-off ch0
			0x91, 0x30, 0x3C, // note-on ch1 48/60
			0x31, 0x33, // running note-on ch1 49/51
			0xB0, 0x64, 0x7F, // CC ch0 ctrl 100 val 127
			0xD2, 0x10, // aftertouch ch2
	};
	static const uint8_t expected_bytes[32] = {
			0x90, 0x3C, 0x64, 0x3D, 0x50, 0xB3, 0x07, 0x7F, 0xF2, 0xC0, 0x2A, 0xE0, 0x40, 0x00,
			0xA2, 0x24, 0x32, 0xD1, 0x2A, 0x80, 0x2E, 0x00, 0x91, 0x30, 0x3C, 0x31, 0x33, 0xB0,
			0x64, 0x7F, 0xD2, 0x10,
	};

	RecordingSink sink;
	MidiFramer framer(sink);
	feed_all(framer, {stream[0], stream[1], stream[2], stream[3], stream[4], stream[5], stream[6],
			stream[7], stream[8], stream[9], stream[10], stream[11], stream[12], stream[13],
			stream[14], stream[15], stream[16], stream[17], stream[18], stream[19], stream[20],
			stream[21], stream[22], stream[23], stream[24], stream[25], stream[26], stream[27],
			stream[28], stream[29], stream[30], stream[31], stream[32], stream[33], stream[34],
			stream[35], stream[36], stream[37], stream[38], stream[39]});

	// on_byte == input minus all 8 sysex bytes, exact order.
	CHECK(sink.bytes == (std::vector<uint8_t>(expected_bytes, expected_bytes + 32)));

	CHECK(sink.sysex.size() == 1);
	if (sink.sysex.size() == 1) {
		CHECK(sink.sysex[0] == (std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0x06, 0x01, 0x01, 0xF7}));
	}
	CHECK(sink.sysex_truncated_calls == 0);

	CHECK(sink.shorts.size() == 12);
	if (sink.shorts.size() == 12) {
		CHECK(sink.shorts[0].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[0].channel == 0);
		CHECK(sink.shorts[0].d1 == 60);
		CHECK(sink.shorts[0].d2 == 100);
		CHECK(sink.shorts[1].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[1].channel == 0);
		CHECK(sink.shorts[1].d1 == 61);
		CHECK(sink.shorts[1].d2 == 80);
		CHECK(sink.shorts[2].kind == MidiKind::CC);
		CHECK(sink.shorts[2].channel == 3);
		CHECK(sink.shorts[2].d1 == 7);
		CHECK(sink.shorts[2].d2 == 127);
		CHECK(sink.shorts[3].kind == MidiKind::PROGRAM_CHANGE);
		CHECK(sink.shorts[3].channel == 0);
		CHECK(sink.shorts[3].d1 == 42);
		CHECK(sink.shorts[4].kind == MidiKind::PITCH_BEND);
		CHECK(sink.shorts[4].channel == 0);
		CHECK(sink.shorts[4].d1 == 0x40);
		CHECK(sink.shorts[4].d2 == 0x00);
		CHECK(sink.shorts[5].kind == MidiKind::POLY_AFTERTOUCH);
		CHECK(sink.shorts[5].channel == 2);
		CHECK(sink.shorts[5].d1 == 0x24);
		CHECK(sink.shorts[5].d2 == 0x32);
		CHECK(sink.shorts[6].kind == MidiKind::AFTERTOUCH);
		CHECK(sink.shorts[6].channel == 1);
		CHECK(sink.shorts[6].d1 == 0x2A);
		CHECK(sink.shorts[7].kind == MidiKind::NOTE_OFF);
		CHECK(sink.shorts[7].channel == 0);
		CHECK(sink.shorts[7].d1 == 0x2E);
		CHECK(sink.shorts[7].d2 == 0);
		CHECK(sink.shorts[8].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[8].channel == 1);
		CHECK(sink.shorts[8].d1 == 0x30);
		CHECK(sink.shorts[8].d2 == 0x3C);
		CHECK(sink.shorts[9].kind == MidiKind::NOTE_ON);
		CHECK(sink.shorts[9].channel == 1);
		CHECK(sink.shorts[9].d1 == 0x31);
		CHECK(sink.shorts[9].d2 == 0x33);
		CHECK(sink.shorts[10].kind == MidiKind::CC);
		CHECK(sink.shorts[10].channel == 0);
		CHECK(sink.shorts[10].d1 == 0x64);
		CHECK(sink.shorts[10].d2 == 0x7F);
		CHECK(sink.shorts[11].kind == MidiKind::AFTERTOUCH);
		CHECK(sink.shorts[11].channel == 2);
		CHECK(sink.shorts[11].d1 == 0x10);
	}
	std::printf("byte_fidelity done\n");
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_note_on_explicit();
	test_note_off_and_vel0();
	test_running_status();
	test_cc();
	test_program_change_2byte();
	test_pitch_bend_14bit();
	test_aftertouch_and_poly();
	test_sysex_whole();
	test_sysex_truncation();
	test_realtime_interleaved();
	test_garbage_no_status();
	test_byte_fidelity();

	if (failures == 0) {
		std::printf("ALL MIDI FRAMER TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", failures);
	return 1;
}

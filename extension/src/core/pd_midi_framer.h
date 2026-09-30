#ifndef GD_LIBPD_MIDI_FRAMER_H
#define GD_LIBPD_MIDI_FRAMER_H

#include <cstdint>

namespace godot_libpd {

// Kinds of short messages the framer can emit.
enum class MidiKind : uint8_t {
	NOTE_ON,
	NOTE_OFF,
	CC,
	PROGRAM_CHANGE,
	PITCH_BEND,
	AFTERTOUCH,
	POLY_AFTERTOUCH,
};

// One decoded MIDI short message. d1/d2 meanings per kind:
// NOTE_ON / NOTE_OFF: d1 = pitch, d2 = velocity (vel-0 NOTE_ON reports as NOTE_OFF)
// CC:                 d1 = controller, d2 = value
// PROGRAM_CHANGE:     d1 = program, d2 unused
// PITCH_BEND:         d1 = low 7 bits, d2 = high 7 bits
// AFTERTOUCH:         d1 = value, d2 unused
// POLY_AFTERTOUCH:    d1 = pitch, d2 = value
struct MidiShortMsg {
	MidiKind kind;
	uint8_t channel;
	uint8_t d1;
	uint8_t d2;
};

// Output sink for MidiFramer. Implementations receive:
// - on_byte: every non-sysex stream byte in original order (status bytes,
//   running-status data, realtime), never bytes inside an F0..F7 sysex
//   (neither F0 nor F7);
// - on_short: each completed short message (vel-0 NOTE_ON reported as NOTE_OFF);
// - on_sysex: the sysex body (bytes between F0 and F7, exclusive of F0,
//   inclusive of F7), capped at 127 bytes;
// - on_sysex_truncated: once, when F7 terminated a sysex that exceeded 127
//   bytes (data holds the first 127).
struct MidiFramingSink {
	virtual void on_short(const MidiShortMsg &p_msg) = 0;
	virtual void on_byte(uint8_t p_byte) = 0;
	virtual void on_sysex(const uint8_t *p_data, int p_len) = 0;
	virtual void on_sysex_truncated() = 0;
	virtual ~MidiFramingSink() = default;
};

// Pure running-status parser over a raw MIDI byte stream. One framer per
// open input port; the sink is held by reference for the framer's lifetime.
// feed() may invoke the sink multiple times per byte (byte pass-through +
// short message on completion). Fixed state only, allocation-free.
class MidiFramer {
public:
	explicit MidiFramer(MidiFramingSink &p_sink);

	void feed(uint8_t p_byte);
	void reset();

private:
	enum State {
		NO_STATUS,
		STATUS_SEEN,
		IN_SYSEX,
	};

	MidiFramingSink &sink;
	State state = NO_STATUS;
	uint8_t status = 0;
	int data_needed = 0;
	int data_have = 0;
	uint8_t data[2];

	static constexpr int SYSEX_MAX = 127;
	uint8_t sysex_buf[SYSEX_MAX];
	int sysex_len = 0;
	bool sysex_truncated = false;
};

} // namespace godot_libpd

#endif // GD_LIBPD_MIDI_FRAMER_H

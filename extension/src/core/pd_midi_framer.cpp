// MidiFramer: pure running-status parser over a raw MIDI byte stream.
// No PortMIDI/libpd/Godot dependencies; fixed state only, allocation-free.
#include "core/pd_midi_framer.h"

namespace godot_libpd {

MidiFramer::MidiFramer(MidiFramingSink &p_sink) : sink(p_sink) {}

void MidiFramer::reset() {
	state = NO_STATUS;
	status = 0;
	data_needed = 0;
	data_have = 0;
	sysex_len = 0;
	sysex_truncated = false;
}

void MidiFramer::feed(uint8_t p_byte) {
	// Inside an F0..F7 sysex: no on_byte for any of the bytes (including
	// F7); the body buffer holds between-F0-and-F7, inclusive of F7.
	if (state == IN_SYSEX) {
		if (p_byte == 0xF7) {
			// F7 terminates and is part of the stored body (dropped only when
			// the 127-byte buffer is already full).
			if (sysex_len < SYSEX_MAX) {
				sysex_buf[sysex_len++] = p_byte;
			} else {
				sysex_truncated = true;
			}
			sink.on_sysex(sysex_buf, sysex_len);
			if (sysex_truncated) {
				sink.on_sysex_truncated();
			}
			reset();
			return;
		}
		if (sysex_len < SYSEX_MAX) {
			sysex_buf[sysex_len++] = p_byte;
		} else {
			sysex_truncated = true;
		}
		return;
	}

	if (p_byte == 0xF0) {
		state = IN_SYSEX;
		sysex_len = 0;
		sysex_truncated = false;
		return;
	}

	if (p_byte >= 0xF1) {
		// System real-time: pass through without disturbing message assembly.
		sink.on_byte(p_byte);
		return;
	}

	if (p_byte >= 0x80) {
		// New (or re-sent) status byte; starts a 2- or 3-byte message.
		status = p_byte;
		sink.on_byte(p_byte);
		const uint8_t type = status & 0xF0;
		// 1 data byte: program change (C0) and channel aftertouch (D0).
		// 2 data bytes: note on/off, CC, poly aftertouch, pitch bend.
		data_needed = (type == 0xC0 || type == 0xD0) ? 1 : 2;
		data_have = 0;
		state = STATUS_SEEN;
		return;
	}

	if (state == NO_STATUS) {
		// Data-only byte before any status: dropped entirely (not emitted).
		return;
	}

	// Data byte of a current/running-status message.
	sink.on_byte(p_byte);
	data[data_have++] = p_byte;
	if (data_have < data_needed) {
		return;
	}

	// Message complete; running status persists for the next data bytes.
	data_have = 0;
	const uint8_t type = status & 0xF0;
	MidiShortMsg msg;
	msg.channel = status & 0x0F;
	if (type == 0x80 || type == 0x90) {
		// Vel-0 note-on is reported as NOTE_OFF.
		msg.kind = (type == 0x80 || data[1] == 0) ? MidiKind::NOTE_OFF : MidiKind::NOTE_ON;
		msg.d1 = data[0]; // pitch
		msg.d2 = data[1]; // velocity
	} else if (type == 0xB0) {
		msg.kind = MidiKind::CC;
		msg.d1 = data[0]; // controller
		msg.d2 = data[1]; // value
	} else if (type == 0xC0) {
		msg.kind = MidiKind::PROGRAM_CHANGE;
		msg.d1 = data[0]; // program
		msg.d2 = 0;
	} else if (type == 0xE0) {
		msg.kind = MidiKind::PITCH_BEND;
		msg.d1 = data[0]; // low 7 bits
		msg.d2 = data[1]; // high 7 bits
	} else if (type == 0xD0) {
		msg.kind = MidiKind::AFTERTOUCH;
		msg.d1 = data[0]; // value
		msg.d2 = 0;
	} else { // 0xA0
		msg.kind = MidiKind::POLY_AFTERTOUCH;
		msg.d1 = data[0]; // pitch
		msg.d2 = data[1]; // value
	}
	sink.on_short(msg);
}

} // namespace godot_libpd

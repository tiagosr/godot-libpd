// M3 Task 0 recon probe (macOS / CoreMIDI).
// Standalone RtMidi probe — compiled against the vendored RtMidi.cpp.
// RtMidi's model is THROW-based: openPort/openVirtualPort throw
// RtMidiError; the input callback receives ONE std::vector<unsigned char>*
// (a complete message, already reassembled for sysex).
//
// Answers, with printed evidence:
//   P1  Does CoreMIDI enumeration see the IAC bus (in + out)?
//   P2  Can we open an input + output port and drive a note?
//   P3  Does openVirtualPort() work on CoreMIDI (in and out)?
//   P4  Does a sysex round-trip reassemble to a full F0..F7 message?
//   P5  Does ignoreTypes(false,false,false) deliver sysex?
#include "RtMidi.h"
#include <cstdio>
#include <vector>
#include <string>

static int g_note = -1;
static std::vector<unsigned char> g_sysex;

static void input_callback(double dt, std::vector<unsigned char> *msg, void *user) {
    (void)dt;
    (void)user;
    if (msg == nullptr || msg->empty()) return;
    unsigned char st = (*msg)[0];
    if ((st & 0xf0) == 0x90 && msg->size() >= 3) {
        g_note = (*msg)[1];
        std::printf("PROBE note_on ch=%u pitch=%u vel=%u (nbytes=%zu)\n",
                    st & 0x0f, (*msg)[1], (*msg)[2], msg->size());
    } else if (st == 0xF0) {
        g_sysex.assign(msg->begin(), msg->end());
        std::printf("PROBE sysex nbytes=%zu first=%02x last=%02x\n",
                    msg->size(), (*msg)[0], msg->back());
    } else {
        std::printf("PROBE msg nbytes=%zu st=%02x\n", msg->size(), st);
    }
}

int main() {
    std::printf("PROBE start\n");

    // --- P1: enumeration -------------------------------------------------
    RtMidiIn in(RtMidi::MACOSX_CORE);
    RtMidiOut out(RtMidi::MACOSX_CORE);
    int inN = in.getPortCount();
    int outN = out.getPortCount();
    std::printf("P1 in_count=%d out_count=%d\n", inN, outN);
    for (int i = 0; i < inN; ++i)
        std::printf("  in[%d] = %s\n", i, in.getPortName(i).c_str());
    for (int i = 0; i < outN; ++i)
        std::printf("  out[%d] = %s\n", i, out.getPortName(i).c_str());
    if (inN == 0 || outN == 0) {
        std::printf("PROBE ABORT no IAC bus visible (need 'IAC Driver Bus')\n");
        return 2;
    }

    // --- P2: open + drive a note ----------------------------------------
    in.setCallback(&input_callback, nullptr);
    in.ignoreTypes(false, false, false); // P5: accept sysex+timing+sensing
    try {
        in.openPort(0, "probe-in");
        std::printf("P2 open_in(0) OK\n");
    } catch (RtMidiError &e) {
        std::printf("P2 open_in(0) THREW: %s\n", e.what());
    }
    try {
        out.openPort(0, "probe-out");
        std::printf("P2 open_out(0) OK\n");
    } catch (RtMidiError &e) {
        std::printf("P2 open_out(0) THREW: %s\n", e.what());
    }
    {
        // a note 60 vel 100 on ch 0 -> 0x90 0x3c 0x64
        std::vector<unsigned char> note{0x90, 0x3c, 0x64};
        // a sysex F0 ... F7
        std::vector<unsigned char> syx{0xF0, 0x00, 0x01, 0x02, 0x03, 0x04, 0xF7};
        try {
            out.sendMessage(&note[0], note.size());
            out.sendMessage(&syx[0], syx.size());
            std::printf("P2 sent note + sysex\n");
        } catch (RtMidiError &e) {
            std::printf("P2 sendMessage THREW: %s\n", e.what());
        }
    }

    // --- P3: openVirtualPort ---------------------------------------------
    try {
        in.openVirtualPort("probe-virt-in");
        std::printf("P3 in.openVirtualPort OK\n");
    } catch (RtMidiError &e) {
        std::printf("P3 in.openVirtualPort THREW: %s\n", e.what());
    }
    try {
        out.openVirtualPort("probe-virt-out");
        std::printf("P3 out.openVirtualPort OK\n");
    } catch (RtMidiError &e) {
        std::printf("P3 out.openVirtualPort THREW: %s\n", e.what());
    }

    // brief settle for the callback thread to deliver
    for (volatile long s = 0; s < 20000000L; ++s) {}
    std::printf("P2/P4 got_note=%d got_sysex_bytes=%zu\n", g_note, g_sysex.size());
    std::printf("PROBE done\n");
    return 0;
}

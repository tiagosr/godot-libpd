// M3 Task 0 recon probe (Linux / ALSA sequencer). Target: A133 (kernel 4.9).
// Standalone RtMidi probe — compiled (cross) against the vendored RtMidi.cpp.
//
// Key A133 fact (from docs/knulli-build.md): the ALSA sequencer exposes NO
// SUBS-capable ports, so RtMidi's getPortCount() is expected to be 0 and the
// ONLY viable loopback path is openVirtualPort() (snd_seq) + aconnect.
// This probe answers, with printed evidence:
//   P1  What does CoreMIDI->ALSA enumeration report (expect 0 ports)?
//   P2  Does openVirtualPort() create an in AND out snd_seq port?
//   P3  Can we write a note + sysex to the virtual OUT port?
//   P4  (run with aconnect wired out->in) does the note+sysex come BACK,
//       and does sysex reassemble to a full F0..F7 message?
#include "RtMidi.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <unistd.h>

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
    std::printf("PROBE start (ALSA)\n");

    RtMidiIn in(RtMidi::LINUX_ALSA);
    RtMidiOut out(RtMidi::LINUX_ALSA);
    int inN = in.getPortCount();
    int outN = out.getPortCount();
    std::printf("P1 in_count=%d out_count=%d\n", inN, outN);
    for (int i = 0; i < inN; ++i)
        std::printf("  in[%d] = %s\n", i, in.getPortName(i).c_str());
    for (int i = 0; i < outN; ++i)
        std::printf("  out[%d] = %s\n", i, out.getPortName(i).c_str());

    // --- P2: openVirtualPort (the only viable path on A133) --------------
    in.setCallback(&input_callback, nullptr);
    in.ignoreTypes(false, false, false);
    try {
        in.openVirtualPort("rtmidi-probe-in");
        std::printf("P2 in.openVirtualPort OK\n");
    } catch (RtMidiError &e) {
        std::printf("P2 in.openVirtualPort THREW: %s\n", e.what());
    }
    try {
        out.openVirtualPort("rtmidi-probe-out");
        std::printf("P2 out.openVirtualPort OK\n");
    } catch (RtMidiError &e) {
        std::printf("P2 out.openVirtualPort THREW: %s\n", e.what());
    }

    // Print the ALSA client/port so the operator can aconnect. The
    // virtual port name is registered in the sequencer; aconnect -l will
    // show "rtmidi-probe-in" / "rtmidi-probe-out" under the app's client.
    std::printf("P2 run: aconnect -l  (find the two rtmidi-probe ports)\n");
    std::printf("P2 run: aconnect <out> <in>   then the loopback below fires\n");

    // Wait before sending so the operator (or a wrapper script) can wire
    // `aconnect <out> <in>` while the ports are live. PROBE_PRE_SEND_MS
    // controls the delay; PROBE_HOLD_MS the post-send settle.
    long pre = 4000;
    if (const char *e = getenv("PROBE_PRE_SEND_MS")) pre = atol(e);
    long hold = 1500;
    if (const char *e = getenv("PROBE_HOLD_MS")) hold = atol(e);
    std::printf("P2 waiting %ld ms for aconnect wiring...\n", pre);
    usleep((useconds_t)(pre * 1000));

    // --- P3: write a note + sysex to the virtual OUT ---------------------
    {
        std::vector<unsigned char> note{0x90, 0x3c, 0x64};
        std::vector<unsigned char> syx{0xF0, 0x00, 0x01, 0x02, 0x03, 0x04, 0xF7};
        try {
            out.sendMessage(&note[0], note.size());
            out.sendMessage(&syx[0], syx.size());
            std::printf("P3 sent note + sysex (loopback only returns if aconnected)\n");
        } catch (RtMidiError &e) {
            std::printf("P3 sendMessage THREW: %s\n", e.what());
        }
    }

    // settle
    usleep((useconds_t)(hold * 1000));
    std::printf("P4 got_note=%d got_sysex_bytes=%zu\n", g_note, g_sysex.size());
    std::printf("PROBE done\n");
    return 0;
}

// Platform MIDI backend factory (v2 M2 Task 1+2, M3 Task 1).
//
//   ANDROID     : RtMidiAndroidBackend (vendored RtMidi,
//                 ANDROID_AMIDI; in-process loopback for device
//                 testing — M2 Task 2)
//   macOS/Linux : RtMidiHostBackend (vendored RtMidi; CoreMIDI on
//                 Apple, ALSA on Linux — M3 Task 1, the default).
//                 MIDI_BACKEND=portmidi (build flag) restores the
//                 M1/M2 PortMIDI backend instead.
//   other hosts : nullptr (no MIDI I/O; the router degrades to the
//                 inert stub of the pre-v2 builds)
//
// The returned backend is uninitialized: the router calls
// initialize() before starting its I/O thread.

#include "midi_backend.h"

#if defined(__ANDROID__)
#include "midi_backend_rtmidi.h"
#elif defined(RTMIDI_HOST_ENABLED)
#include "midi_backend_rtmidi_host.h"
#elif defined(PORTMIDI_ENABLED)
#include "midi_backend_portmidi.h"
#endif

#include <memory>

namespace godot_libpd {

std::unique_ptr<MidiBackend> create_midi_backend() {
#if defined(__ANDROID__)
	return std::make_unique<RtMidiAndroidBackend>();
#elif defined(RTMIDI_HOST_ENABLED)
	return std::make_unique<RtMidiHostBackend>();
#elif defined(PORTMIDI_ENABLED)
	return std::make_unique<PortMidiBackend>();
#else
	// No platform backend in this build (stub / hosts without MIDI):
	// the router degrades to the inert stub, as the pre-v2 builds
	// did.
	return nullptr;
#endif
}

} // namespace godot_libpd

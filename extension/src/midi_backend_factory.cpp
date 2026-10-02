// Platform MIDI backend factory (v2 M2, Task 1+2).
//
//   macOS/Linux : PortMidiBackend (vendored PortMIDI 2.0.7,
//                 BUILD_PORTMIDI=ON; the A133 uses its virtual
//                 ports — see the bounds-validated open path in
//                 midi_backend_portmidi.cpp)
//   ANDROID     : RtMidiAndroidBackend (vendored RtMidi,
//                 ANDROID_AMIDI; in-process loopback for device
//                 testing — Task 2)
//   other hosts : nullptr (no MIDI I/O; the router degrades to the
//                 inert stub of the pre-v2 builds)
//
// The returned backend is uninitialized: the router calls
// initialize() before starting its I/O thread.

#include "midi_backend.h"

#if defined(PORTMIDI_ENABLED)
#include "midi_backend_portmidi.h"
#endif
#if defined(__ANDROID__)
#include "midi_backend_rtmidi.h"
#endif

#include <memory>

namespace godot_libpd {

std::unique_ptr<MidiBackend> create_midi_backend() {
#if defined(PORTMIDI_ENABLED)
	return std::make_unique<PortMidiBackend>();
#elif defined(__ANDROID__)
	return std::make_unique<RtMidiAndroidBackend>();
#else
	// No platform backend in this build (stub / hosts without MIDI):
	// the router degrades to the inert stub, as the pre-v2 builds
	// did.
	return nullptr;
#endif
}

} // namespace godot_libpd

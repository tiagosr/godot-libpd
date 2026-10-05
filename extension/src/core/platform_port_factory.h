#pragma once

#include <memory>

#include "audio_port.h"

namespace godot_libpd {

/**
 * Creates the platform's AudioPort implementation:
 *  - Android: OpenSLESPort (OpenSL ES; the only working audio API on the
 *    target device — AAudio is silent there, M6).
 *  - Everything else: PortAudioPort (CoreAudio on macOS, ALSA on Linux).
 *
 * Returns a freshly constructed, closed port.
 */
std::unique_ptr<AudioPort> create_platform_port();

} // namespace godot_libpd

#include "platform_port_factory.h"

#ifdef __ANDROID__
#include "opensles_port.h"
namespace godot_libpd {
std::unique_ptr<AudioPort> create_platform_port() {
	return std::make_unique<OpenSLESPort>();
}
} // namespace godot_libpd
#else
#include "portaudio_port.h"
namespace godot_libpd {
std::unique_ptr<AudioPort> create_platform_port() {
	return std::make_unique<PortAudioPort>();
}
} // namespace godot_libpd
#endif

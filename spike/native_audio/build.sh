#!/bin/sh
# Build the native-audio spike (macOS):
#   vendored PortAudio (coreaudio backend) + vendored libpd + spike.c
#
# Requires the extension macOS build to exist (for libpd-multi.a):
#   cd extension && ./build.sh --macos
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
PA="$ROOT/extension/thirdparty/libpd/pure-data/portaudio/portaudio"
LIBPD_A="$ROOT/extension/build/cmake-macos/thirdparty/libpd/libs/libpd-multi.a"

[ -f "$LIBPD_A" ] || { echo "missing $LIBPD_A (build the extension macOS target first: cd extension && ./build.sh --macos)"; exit 1; }
[ -d "$PA/src/hostapi/coreaudio" ] || { echo "missing vendored PortAudio coreaudio backend"; exit 1; }

clang -O2 -Wall -DPA_USE_COREAUDIO=1 \
	-I"$PA/include" \
	-I"$PA/src" \
	-I"$PA/src/common" \
	-I"$ROOT/extension/thirdparty/libpd/libpd_wrapper" \
	-I"$ROOT/extension/thirdparty/libpd/pure-data/src" \
	spike.c \
	"$PA"/src/common/*.c \
	"$PA"/src/os/unix/*.c \
	"$PA"/src/hostapi/coreaudio/*.c \
	"$LIBPD_A" \
	-lpthread \
	-framework CoreAudio -framework CoreFoundation -framework CoreServices -framework AudioToolbox \
	-o native_audio_spike

echo "built: $(pwd)/native_audio_spike"

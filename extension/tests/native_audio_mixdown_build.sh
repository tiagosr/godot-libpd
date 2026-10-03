#!/bin/sh
# M5 Task 8 — build the NativeAudio mix-down host test (CMake target
# native_audio_mixdown_tests) and stage the binary at
# extension/tests/native_audio_mixdown for the verification protocol:
#   sh extension/tests/native_audio_mixdown_build.sh
#   extension/tests/native_audio_mixdown
set -e
cd "$(dirname "$0")/../.."
if [ ! -f extension/build/cmake-macos/build.ninja ]; then
	cmake -S extension -B extension/build/cmake-macos
fi
cmake --build extension/build/cmake-macos --target native_audio_mixdown_tests
cp extension/build/cmake-macos/native_audio_mixdown_tests extension/tests/native_audio_mixdown
echo "OK: built extension/tests/native_audio_mixdown"

#!/bin/sh
# M5 Task 6/8 — build the server audio host test (CMake target
# server_audio_tests) and stage the binary at extension/tests/server_audio
# for the verification protocol:
#   sh extension/tests/server_audio_build.sh
#   extension/tests/server_audio
set -e
cd "$(dirname "$0")/../.."
if [ ! -f extension/build/cmake-macos/build.ninja ]; then
	cmake -S extension -B extension/build/cmake-macos
fi
cmake --build extension/build/cmake-macos --target server_audio_tests
cp extension/build/cmake-macos/server_audio_tests extension/tests/server_audio
echo "OK: built extension/tests/server_audio"

#!/bin/sh
# Build the godot-libpd GDExtension.
#
#   ./build.sh --macos          (native, host)
#   ./build.sh --linux-arm64    (native aarch64 host — run inside the Knulli Docker image; Task 8)
#   ./build.sh --android        (NDK arm64-v8a — run inside the Android Docker image; Task 10)
set -e
cd "$(dirname "$0")"

# godot-cpp MUST be generated for the `template_release` target, NOT the
# default `template_debug`. The debug target compiles godot-cpp with
# DEBUG_ENABLED; a DEBUG_ENABLED extension loaded into a *release* export
# template (no DEBUG_ENABLED) corrupts the heap during class registration
# (signal/method binding) and segfaults — Task 7 finding. The template_release
# dylib also loads fine in the editor and the debug template, so one build
# serves all three.
GODOTCPP_FLAGS="-DGODOTCPP_TARGET=template_release"

configure() {
	local dir="$1"; shift
	local platform_dir="$1"; shift
	cmake -B "build/cmake-${platform_dir}" -G Ninja $GODOTCPP_FLAGS "$@"
}

build() {
	local dir="$1"
	cmake --build "$dir" --parallel
}

case "${1:-}" in
	--macos)
		configure build/cmake-macos macos -DCMAKE_BUILD_TYPE=Release
		build build/cmake-macos
		;;
	--linux-arm64)
		# Task 8: plain host build with aarch64 gcc (no NDK).
		configure build/cmake-linux linux -DCMAKE_BUILD_TYPE=Release
		build build/cmake-linux
		;;
	--android)
		# Task 10: requires NDK in $ANDROID_NDK (or /opt/android-ndk).
		if [ -z "$NDK" ]; then
			NDK="${ANDROID_NDK:-/opt/android-ndk}"
		fi
		if [ ! -d "$NDK" ]; then
			echo "error: NDK not found (set NDK or ANDROID_NDK)" >&2
			exit 1
		fi
		configure build/cmake-android android-arm64 \
			-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
			-DCMAKE_ANDROID_NDK="$NDK" \
			-DCMAKE_ANDROID_STL_TYPE=c++_shared \
			-DANDROID_ABI=arm64-v8a \
			-DANDROID_PLATFORM=android-29 \
			-DCMAKE_BUILD_TYPE=Release \
			-DBUILD_PORTMIDI=OFF
		build build/cmake-android-arm64
		;;
	*)
		echo "usage: $0 --macos | --linux-arm64 | --android" >&2
		exit 1
		;;
esac

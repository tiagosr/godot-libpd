cmake_minimum_required(VERSION 3.16)
project(godot_libpd CXX)

# ---------------------------------------------------------------------------
# Platform naming for output dir
# ---------------------------------------------------------------------------
if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
    set(LIBPD_PLATFORM_DIR "android-arm64")
elseif(APPLE)
    set(LIBPD_PLATFORM_DIR "macos")
elseif(ANDROID)
    set(LIBPD_PLATFORM_DIR "android-${ANDROID_ABI}")
else()
    set(LIBPD_PLATFORM_DIR "linux")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# godot-cpp (static)
# ---------------------------------------------------------------------------
add_subdirectory(thirdparty/godot-cpp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# libpd (+ pd) static, multi-instance
# ---------------------------------------------------------------------------
set(PD_MULTI ON CACHE BOOL "" FORCE)
set(PD_UTILS OFF CACHE BOOL "" FORCE)
set(PD_EXTRA OFF CACHE BOOL "" FORCE)
set(LIBPD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBPD_STATIC ON CACHE BOOL "" FORCE)
set(PD_BUILD_C_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/libpd EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# PortMidi static (v1: compiled in, not wired up; no Android backend)
# ---------------------------------------------------------------------------
option(BUILD_PORTMIDI "Compile PortMIDI into the extension" ON)
if(ANDROID)
    set(BUILD_PORTMIDI OFF)
endif()
if(BUILD_PORTMIDI)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_PORTMIDI_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/portmidi EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# The GDExtension
# ---------------------------------------------------------------------------
add_library(godot_libpd SHARED
    src/register_types.cpp
    src/libpd_server.cpp
    src/libpd_instance.cpp
    src/libpd_worker.cpp
    src/core/pd_event_ring.cpp
    src/core/pd_command_queue.cpp
    src/core/pd_debug.cpp
    src/core/pd_audio_sink_generator.cpp
)

set_target_properties(godot_libpd PROPERTIES
    OUTPUT_NAME "godot_libpd"
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/${LIBPD_PLATFORM_DIR}"
)

target_include_directories(godot_libpd PRIVATE
    src
    thirdparty/libpd/libpd_wrapper
    thirdparty/libpd/libpd_wrapper/util
)

target_link_libraries(godot_libpd PRIVATE godot-cpp libpd_static)
if(BUILD_PORTMIDI)
    target_link_libraries(godot_libpd PRIVATE portmidi)
endif()

# ---------------------------------------------------------------------------
# Host unit tests (Task 2; off for cross builds)
# ---------------------------------------------------------------------------
option(BUILD_LIBPD_TESTS "Build host unit tests" ON)
if(ANDROID)
    set(BUILD_LIBPD_TESTS OFF)
endif()
if(BUILD_LIBPD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(libpd_core_tests
        tests/core_tests.cpp
        src/core/pd_event_ring.cpp
        src/core/pd_command_queue.cpp
    )
    target_include_directories(libpd_core_tests PRIVATE src)
    target_link_libraries(libpd_core_tests PRIVATE pthread)
    add_test(NAME core_tests COMMAND $<TARGET_FILE:libpd_core_tests>)
endif()

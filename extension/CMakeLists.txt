cmake_minimum_required(VERSION 3.16)
project(godot_libpd C CXX)

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
# Multi-instance MIDI hook attribution REQUIRES PD_MULTI: only then is
# each worker's instancedata/hook state per instance (thread-local
# pd_this); PD_MULTI OFF falls back to one process-wide libpd_mainimp
# and every instance's hooks misroute to the last-initialized worker.
# See README "Multi-instance MIDI hook attribution" and tests/
# multi_instance_midi_tests.cpp. Do not turn this OFF.
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
    src/midi_router.cpp
    src/core/pd_event_ring.cpp
    src/core/pd_command_queue.cpp
    src/core/pd_midi_framer.cpp
    src/core/pd_debug.cpp
    src/core/pd_audio_sink_generator.cpp
)
if(ANDROID)
    # bionic lacks glob(3) below API 28; libpd links it (see file header).
    target_sources(godot_libpd PRIVATE src/compat/android_glob_stub.c)
endif()

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
    # MidiRouter compiles against the vendored PortMIDI 2.0.7 only when
    # the backend is built in; otherwise it degrades to inert stubs.
    target_compile_definitions(godot_libpd PRIVATE PORTMIDI_ENABLED)
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
        src/core/pd_midi_framer.cpp
    )
    target_include_directories(libpd_core_tests PRIVATE src)
    target_link_libraries(libpd_core_tests PRIVATE pthread)
    add_test(NAME core_tests COMMAND $<TARGET_FILE:libpd_core_tests>)

    add_executable(midi_framer_tests
        tests/midi_framer_tests.cpp
        src/core/pd_midi_framer.cpp
    )
    target_include_directories(midi_framer_tests PRIVATE src)
    add_test(NAME midi_framer_tests COMMAND $<TARGET_FILE:midi_framer_tests>)

    add_executable(midi_routing_tests
        tests/midi_routing_tests.cpp
    )
    target_include_directories(midi_routing_tests PRIVATE src)
    target_link_libraries(midi_routing_tests PRIVATE pthread)
    add_test(NAME midi_routing_tests COMMAND $<TARGET_FILE:midi_routing_tests>)

    # Task 4: MidiOutWriter is pure (no PortMIDI, no threads) and lives
    # inline in midi_router.h, so this target needs no portmidi include
    # path and no .cpp to link.
    add_executable(midi_writer_tests
        tests/midi_writer_tests.cpp
    )
    target_include_directories(midi_writer_tests PRIVATE src)
    add_test(NAME midi_writer_tests COMMAND $<TARGET_FILE:midi_writer_tests>)

    # Task 4 rework: MidiReadStage (per-port sysex read stage) is pure
    # (no PortMIDI, no threads) and lives inline in midi_router.h; the
    # framer is linked only for the end-to-end cases.
    add_executable(midi_read_stage_tests
        tests/midi_read_stage_tests.cpp
        src/core/pd_midi_framer.cpp
    )
    target_include_directories(midi_read_stage_tests PRIVATE src)
    add_test(NAME midi_read_stage_tests COMMAND $<TARGET_FILE:midi_read_stage_tests>)

    if(BUILD_PORTMIDI)
        # Task 7: PortMIDI virtual device lifecycle (create -> open ->
        # write -> close -> delete). Host API only; delivery is proven
        # on-device (A133 aconnect loopback, docs/knulli-build.md).
        add_executable(midi_virtual_port_tests
            tests/midi_virtual_port_tests.cpp
        )
        target_include_directories(midi_virtual_port_tests PRIVATE src)
        target_link_libraries(midi_virtual_port_tests PRIVATE portmidi)
        add_test(NAME midi_virtual_port_tests COMMAND $<TARGET_FILE:midi_virtual_port_tests>)

        # A133 freeze regression (out-of-range PM index on open):
        # fails fast with the port_error signal; the in-test watchdog
        # turns a hang regression into a ctest failure instead of
        # hanging ctest. Links the router itself (the validated layer).
        add_executable(midi_open_bounds_tests
            tests/midi_open_bounds_tests.cpp
            src/midi_router.cpp
            src/core/pd_midi_framer.cpp
        )
        target_include_directories(midi_open_bounds_tests PRIVATE src)
        target_compile_definitions(midi_open_bounds_tests PRIVATE PORTMIDI_ENABLED)
        target_link_libraries(midi_open_bounds_tests PRIVATE portmidi pthread)
        add_test(NAME midi_open_bounds_tests COMMAND $<TARGET_FILE:midi_open_bounds_tests>)
    endif()

    add_executable(libpd_worker_midi_tests
        tests/worker_midi_tests.cpp
        src/libpd_worker.cpp
        src/core/pd_command_queue.cpp
        src/core/pd_debug.cpp
        src/core/pd_audio_sink_generator.cpp
    )
    target_include_directories(libpd_worker_midi_tests PRIVATE
        src
        thirdparty/libpd/libpd_wrapper
        thirdparty/libpd/libpd_wrapper/util
    )
    target_link_libraries(libpd_worker_midi_tests PRIVATE libpd_static godot-cpp pthread)
    add_test(NAME worker_midi_tests COMMAND $<TARGET_FILE:libpd_worker_midi_tests>)

    # Multi-instance regression (P1-1 investigated, disproven): per-instance
    # hook attribution with two real workers + routed fan-out A -> port -> B
    # at the hook level. The fan-out uses the pure header-only
    # MidiRoutingTable plus a local mirror of MidiRouter::build_command, so
    # no PortMIDI I/O ever happens (same sources as libpd_worker_midi_tests).
    add_executable(multi_instance_midi_tests
        tests/multi_instance_midi_tests.cpp
        src/libpd_worker.cpp
        src/core/pd_command_queue.cpp
        src/core/pd_debug.cpp
        src/core/pd_audio_sink_generator.cpp
    )
    target_include_directories(multi_instance_midi_tests PRIVATE
        src
        thirdparty/libpd/libpd_wrapper
        thirdparty/libpd/libpd_wrapper/util
    )
    target_link_libraries(multi_instance_midi_tests PRIVATE libpd_static godot-cpp pthread)
    add_test(NAME multi_instance_midi_tests COMMAND $<TARGET_FILE:multi_instance_midi_tests>)
endif()

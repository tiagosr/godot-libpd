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
# RtMidi static (the ONLY MIDI backend). Android: the RtMidiAndroidBackend
# (AMIDI; the local fixes are pinned at the submodule commit). Hosts
# (macOS/Linux): the RtMidiHostBackend — CoreMIDI on Apple, ALSA on Linux.
# (PortMIDI was retired as the in-project backend 2026-10; the upstream
# patchset remains at patches/portmidi-upstream/.)
# ---------------------------------------------------------------------------
set(RTMIDI_HOST OFF)
if(ANDROID)
    set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/rtmidi EXCLUDE_FROM_ALL)
else()
    set(RTMIDI_HOST ON)
    set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    # Keep the RtMidi sub-tree quiet: no RtMidi demo/test programs, no
    # install rules, no ALSA hotplug build-time check (those belong to
    # RtMidi's own test suite, not to the extension build).
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTMIDI_BUILD_HOTPLUG_TEST OFF CACHE BOOL "" FORCE)
    set(RTMIDI_API_JACK OFF CACHE BOOL "" FORCE)
    set(RTMIDI_API_WINMM OFF CACHE BOOL "" FORCE)
    set(RTMIDI_API_AMIDI OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(RTMIDI_API_CORE ON CACHE BOOL "" FORCE)
    else()
        set(RTMIDI_API_ALSA ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(thirdparty/rtmidi EXCLUDE_FROM_ALL)
    # The static rtmidi archive must be position-independent: it links
    # into the shared GDExtension. arm64 is strict about non-PIC input
    # ("dangerous relocation" at link); force -fPIC on the target.
    target_compile_options(rtmidi PRIVATE -fPIC)
endif()

# ---------------------------------------------------------------------------
# Native audio (M5 + M6'): the mix-down render path is compiled into the
# extension on ALL platforms (NATIVE_AUDIO ON). The backend AudioPort is
# platform-specific: PortAudio (CoreAudio on macOS, ALSA on Linux) built in
# place from the pd-bundled copy (no Android hostapi upstream), OpenSL ES on
# Android (NDK-shipped headers + libOpenSLES). PortAudio is NOT built on
# Android.
# ---------------------------------------------------------------------------
set(NATIVE_AUDIO ON)
if(NOT ANDROID)
    set(PA_ROOT thirdparty/libpd/pure-data/portaudio/portaudio)
    file(GLOB PA_COMMON_SOURCES ${PA_ROOT}/src/common/*.c)
    file(GLOB PA_UNIX_SOURCES ${PA_ROOT}/src/os/unix/*.c)
    if(APPLE)
        file(GLOB PA_HOST_SOURCES ${PA_ROOT}/src/hostapi/coreaudio/*.c)
        set(PA_HOST_DEFINE PA_USE_COREAUDIO=1)
    else()
        file(GLOB PA_HOST_SOURCES ${PA_ROOT}/src/hostapi/alsa/*.c)
        set(PA_HOST_DEFINE PA_USE_ALSA=1)
    endif()
    add_library(portaudio_lib STATIC
        ${PA_COMMON_SOURCES} ${PA_UNIX_SOURCES} ${PA_HOST_SOURCES})
    target_include_directories(portaudio_lib PUBLIC
        ${PA_ROOT}/include ${PA_ROOT}/src ${PA_ROOT}/src/common)
    target_compile_definitions(portaudio_lib PRIVATE ${PA_HOST_DEFINE})
    if(APPLE)
        target_link_libraries(portaudio_lib PRIVATE
            "-framework CoreAudio" "-framework CoreFoundation"
            "-framework CoreServices" "-framework AudioToolbox")
    else()
        target_link_libraries(portaudio_lib PRIVATE asound)
    endif()
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
    src/midi_backend_rtmidi.cpp
    src/midi_backend_rtmidi_host.cpp
    src/midi_backend_factory.cpp
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
if(ANDROID)
    target_link_libraries(godot_libpd PRIVATE rtmidi)
endif()
if(RTMIDI_HOST)
    target_link_libraries(godot_libpd PRIVATE rtmidi)
    # The static rtmidi archive does not propagate its platform link
    # requirements; the shared extension must carry them:
    #   macOS : the CoreMIDI stack frameworks
    #   Linux : libasound (ALSA sequencer)
    if(APPLE)
        target_link_libraries(godot_libpd PRIVATE
            "-framework CoreMIDI" "-framework CoreAudio"
            "-framework CoreFoundation" "-framework CoreServices")
    else()
        target_link_libraries(godot_libpd PRIVATE asound)
    endif()
    target_compile_definitions(godot_libpd PRIVATE RTMIDI_HOST_ENABLED)
endif()

# M5/M6' native audio: the mix-down render path + the platform AudioPort
# (PortAudio host, OpenSL ES on Android).
if(NATIVE_AUDIO)
    target_sources(godot_libpd PRIVATE
        src/core/native_audio.cpp
        src/core/platform_port_factory.cpp)
    target_compile_definitions(godot_libpd PRIVATE NATIVE_AUDIO)
    if(ANDROID)
        target_sources(godot_libpd PRIVATE src/core/opensles_port.cpp)
        target_link_libraries(godot_libpd PRIVATE openSLES)
    else()
        target_sources(godot_libpd PRIVATE src/core/portaudio_port.cpp)
        target_link_libraries(godot_libpd PRIVATE portaudio_lib)
        if(APPLE)
            target_link_libraries(godot_libpd PRIVATE
                "-framework CoreAudio" "-framework CoreFoundation"
                "-framework CoreServices" "-framework AudioToolbox")
        else()
            target_link_libraries(godot_libpd PRIVATE asound)
        endif()
    endif()
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

    # v2 M2 Task 1: the MidiBackend contract through a fake backend —
    # no platform MIDI, no device (the Android RtMidi backend must
    # satisfy the same contract; the virtual-loopback case is the
    # on-device test path there).
    add_executable(midi_backend_fake_tests
        tests/midi_backend_fake_tests.cpp
        src/midi_router.cpp
        src/midi_backend_factory.cpp
        src/core/pd_midi_framer.cpp
        src/core/pd_command_queue.cpp
    )
    target_include_directories(midi_backend_fake_tests PRIVATE src)
    target_link_libraries(midi_backend_fake_tests PRIVATE pthread)
    add_test(NAME midi_backend_fake_tests COMMAND $<TARGET_FILE:midi_backend_fake_tests>)

    # v2 M2 Task 2: the RtMidi Android backend's platform-free seams
    # (word chop, device-position translation, word ring) — header-only
    # logic, tested on every host; the AMIDI glue itself is verified
    # on device (M2 Task 3/4).
    add_executable(midi_rtmidi_seam_tests
        tests/midi_rtmidi_seam_tests.cpp
    )
    target_include_directories(midi_rtmidi_seam_tests PRIVATE src)
    target_link_libraries(midi_rtmidi_seam_tests PRIVATE pthread)
    add_test(NAME midi_rtmidi_seam_tests COMMAND $<TARGET_FILE:midi_rtmidi_seam_tests>)

    if(RTMIDI_HOST)
        # v2 M3 Task 1: the RtMidi host backend (CoreMIDI/ALSA) — the
        # in-process loopback round-trip (create_virtual_loopback ->
        # open 200/201 -> write note + sysex -> poll words) is fully
        # device-free; a host with no usable MIDI server (initialize()
        # -> Unavailable) reports skip (exit 0) so headless CI stays
        # green. Real-device legs: macOS IAC smoke + A133 aconnect
        # (M3 Task 2).
        add_executable(midi_rtmidi_host_backend_tests
            tests/midi_rtmidi_host_backend_tests.cpp
            src/midi_backend_rtmidi_host.cpp
        )
        target_include_directories(midi_rtmidi_host_backend_tests PRIVATE
            src thirdparty/rtmidi)
        target_link_libraries(midi_rtmidi_host_backend_tests PRIVATE
            rtmidi pthread)
        target_compile_definitions(midi_rtmidi_host_backend_tests PRIVATE
            RTMIDI_HOST_ENABLED)
        if(APPLE)
            target_link_libraries(midi_rtmidi_host_backend_tests PRIVATE
                "-framework CoreMIDI" "-framework CoreAudio"
                "-framework CoreFoundation" "-framework CoreServices")
        else()
            target_link_libraries(midi_rtmidi_host_backend_tests PRIVATE asound)
        endif()
        add_test(NAME midi_rtmidi_host_backend_tests
            COMMAND $<TARGET_FILE:midi_rtmidi_host_backend_tests>)
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

    # Receive hooks (patch -> host messages): the worker's BANG/FLOAT/SYMBOL
    # PdEvent emission via the libpd receive hooks (real instance, no DSP).
    add_executable(libpd_message_hook_tests
        tests/message_hook_tests.cpp
        src/libpd_worker.cpp
        src/core/pd_command_queue.cpp
        src/core/pd_debug.cpp
        src/core/pd_audio_sink_generator.cpp
    )
    target_include_directories(libpd_message_hook_tests PRIVATE
        src
        thirdparty/libpd/libpd_wrapper
        thirdparty/libpd/libpd_wrapper/util
    )
    target_link_libraries(libpd_message_hook_tests PRIVATE libpd_static godot-cpp pthread)
    add_test(NAME message_hook_tests COMMAND $<TARGET_FILE:libpd_message_hook_tests>)

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

    # M5 native audio. Pure (no godot-cpp): libpd + the core C++ + PortAudio.
    if(NATIVE_AUDIO)
        # T3: per-synth-worker ring (Option B: gather_latest_n). No libpd.
        add_executable(mix_input_ring_tests
            tests/mix_input_ring_tests.cpp
        )
        target_include_directories(mix_input_ring_tests PRIVATE src)
        target_link_libraries(mix_input_ring_tests PRIVATE pthread)
        add_test(NAME mix_input_ring_tests COMMAND $<TARGET_FILE:mix_input_ring_tests>)

        # T4: mix-down render in the PortAudio callback + 9-concurrent guard.
        add_executable(native_audio_mixdown_tests
            tests/native_audio_mixdown_tests.cpp
            src/core/native_audio.cpp
        )
        target_include_directories(native_audio_mixdown_tests PRIVATE
            src
            thirdparty/libpd/libpd_wrapper
            thirdparty/libpd/libpd_wrapper/util
            thirdparty/libpd/pure-data/src
        )
        target_compile_definitions(native_audio_mixdown_tests PRIVATE NATIVE_AUDIO)
        target_link_libraries(native_audio_mixdown_tests PRIVATE libpd_static portaudio_lib pthread)
        if(APPLE)
            target_link_libraries(native_audio_mixdown_tests PRIVATE
                "-framework CoreAudio" "-framework CoreFoundation"
                "-framework CoreServices" "-framework AudioToolbox")
        endif()
        add_test(NAME native_audio_mixdown_tests COMMAND $<TARGET_FILE:native_audio_mixdown_tests>)

        # T5: worker roles (SYNTH ring sink / MIXER control-only / ANDROID).
        add_executable(native_worker_tests
            tests/native_worker_tests.cpp
            src/libpd_worker.cpp
            src/core/pd_command_queue.cpp
            src/core/pd_debug.cpp
        )
        target_include_directories(native_worker_tests PRIVATE
            src
            thirdparty/libpd/libpd_wrapper
            thirdparty/libpd/libpd_wrapper/util
        )
        target_compile_definitions(native_worker_tests PRIVATE NATIVE_AUDIO)
        target_link_libraries(native_worker_tests PRIVATE libpd_static pthread)
        add_test(NAME native_worker_tests COMMAND $<TARGET_FILE:native_worker_tests>)

        # T6: server audio API + ring wiring (Option B end-to-end, NullPort).
        add_executable(server_audio_tests
            tests/server_audio_tests.cpp
            src/core/native_audio.cpp
            src/libpd_worker.cpp
            src/core/pd_command_queue.cpp
            src/core/pd_debug.cpp
        )
        target_include_directories(server_audio_tests PRIVATE
            src
            thirdparty/libpd/libpd_wrapper
            thirdparty/libpd/libpd_wrapper/util
            thirdparty/libpd/pure-data/src
        )
        target_compile_definitions(server_audio_tests PRIVATE NATIVE_AUDIO)
        target_link_libraries(server_audio_tests PRIVATE libpd_static portaudio_lib pthread)
        if(APPLE)
            target_link_libraries(server_audio_tests PRIVATE
                "-framework CoreAudio" "-framework CoreFoundation"
                "-framework CoreServices" "-framework AudioToolbox")
        endif()
        add_test(NAME server_audio_tests COMMAND $<TARGET_FILE:server_audio_tests>)
    endif()
endif()

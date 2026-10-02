# Android build & test (GDExtension)

No engine build is needed for Android — use the official 4.6.2 templates and
ship the GDExtension as an `.so` inside the APK.

## 1. Build the extension (NDK)

    extension/build.sh --android

Produces `extension/build/android-arm64/libgodot_libpd.so` (aarch64).

Build facts:
- NDK 25.1.8937393, `android-21` (API 21) target, `c++_shared` STL —
  `libc++_shared.so` is packaged into the APK automatically by the exporter.
- bionic has no `glob(3)` until API 28. `src/compat/android_glob_stub.c`
  provides a guarded no-op stub for `__ANDROID_API__ < 28` (pd never calls
  glob; the stub exists only to satisfy the linker).
- godot-cpp is configured with `GODOTCPP_TARGET=template_release` like every
  other platform (debug-target godot-cpp corrupts the release template heap).

## 2. Export the APK

Editor settings (Godot 4.6 macOS path) need the SDK locations:
`~/Library/Application Support/Godot/editor_settings-4.6.tres`:

    export/android/android_sdk_path = "<sdk>"
    export/android/java_sdk_path    = "<jdk home>"

Then (official 4.6.2 templates):

    Godot --headless --path test_project --export-debug   Android dist/android/godot-libpd-test-debug.apk
    Godot --headless --path test_project --export-release Android dist/android/godot-libpd-test-release.apk

Signing:
- Debug APK: signed automatically with the editor debug keystore.
- Release APK: requires a keystore via preset options
  `keystore/release`, `keystore/release_user`, `keystore/release_password`.
  A dev keystore is generated at `dist/android/godot-libpd-dev.keystore`
  (alias `godotlibpd`, password `android`):

      keytool -genkeypair -keystore dist/android/godot-libpd-dev.keystore \
          -alias godotlibpd -keyalg RSA -keysize 2048 -validity 10000 \
          -storepass android -keypass android \
          -dname "CN=dev, OU=dev, O=dev, L=dev, ST=dev, C=US"

  Replace it with a real keystore for store distribution.

## 3. CLI arguments on Android

`am start -- args` does **not** reach the engine. Godot reads the command
line from `assets/_cl_` inside the APK, which the exporter generates from
the preset option `command_line/extra_args` (space-separated string).

The test preset bakes in `--headless --smoke`, so launching the app runs the
headless smoke test and quits with exit code 0. For a GUI build, export with
an empty `command_line/extra_args`.

## 4. Run on device

    adb install -r dist/android/godot-libpd-test-debug.apk
    adb shell am start -n org.godotlibpd.test/com.godot.game.GodotApp
    adb logcat -d | grep SMOKE          # expect: SMOKE_OK

The device screen must be **on/unlocked**: even `--headless` needs the
activity foregrounded on Android (with the screen off the activity is
created in the background and the engine waits for a surface that never
arrives).

## Verified

Retroid "RG DS" (API 34, arm64-v8a): debug and release APKs both `SMOKE_OK`
(engine 4.6.2 official template, extension from `lib/arm64-v8a/`, all six
smoke steps including MIDI round-trip and two concurrent instances).

## 5. Gradle build + Android MIDI (v2 M2)

Since M2 the Android preset uses the **gradle source build** (needed to
compile the RtMidi `MidiDeviceOpenedListener` Java class into the APK):

- `test_project/android/build/` — the official 4.6.2 `android_source.zip`
  template (installed layout: build dir = `res://android/build`, marker
  `res://android/.build_version`).
- `test_project/android/build/src/main/java/com/yellowlab/rtmidi/MidiDeviceOpenedListener.java`
  — RtMidi's required listener (from `extension/thirdparty/rtmidi/contrib/java/`).
- Preset options: `gradle_build/use_gradle_build=true`,
  `gradle_build/min_sdk=29`, `gradle_build/target_sdk=35`
  (minSdk 29 because the NDK **AMidi** C API is API 29+; the Java MIDI
  API itself is 26).
- Export: `--export-debug Android ...` runs the gradle build; the APK
  lands in `test_project/android/build/build/outputs/apk/standard/debug/`
  (gradle build dir is gitignored).
- The device screen must be **awake/unlocked** for the app to run
  (even `--headless`): `adb shell input keyevent KEYCODE_WAKEUP` before
  launching.
- RtMidi NDK cross-compile facts (NDK 25.1.8937393): legacy toolchain
  vars (`ANDROID_ABI=arm64-v8a ANDROID_PLATFORM=android-29
  ANDROID_STL=c++_shared`), `RTMIDI_API_AMIDI=ON`; RtMidi's CMake omits
  `-ljvm` (see the JavaVM note below — the NDK ships no linkable
  libart/jvm).

### How the extension gets a `JavaVM*` (Android 14, on-device verified)

An Android app's linker namespace ("clns-N") blocks every obvious route
to the JVM from native code:

* `dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")` → NULL (the VM is not in
  the global symbol scope of an app process);
* `dlopen("/apex/com.android.art/lib64/libart.so")` → *"not accessible
  for the namespace clns-N"*;
* `dlopen("libart.so")` / `dlopen("/system/lib64/libart.so")` → not found;
* a GDExtension `.so` is loaded by the engine with a plain C++ `dlopen`, so
  `JNI_OnLoad` is **never** called — there is no native bootstrap hook.

The working solution (implemented in `midi_backend_rtmidi.cpp`) resolves
`JNI_GetCreatedJavaVMs` by parsing libart.so's own ELF dynamic symbol
table through `/proc/self/maps` + `/proc/self/mem`: libart's TEXT is mapped
into the app's address space (it *is* the JVM) and `/proc/self/mem` is
readable by the app. The pointer is only called after verifying it sits
inside an `r-x` libart mapping (the guard caught a real struct-size
misparse before it could execute a bad pointer). The vendored RtMidi's own
`androidGetThreadEnv()` (used when opening a *real* MIDI port) reuses that
result via the host hook `gdpd_rtmidi_host_java_vm()` — an `extern "C"`
symbol in the same `.so`, reachable through `dlsym(RTLD_DEFAULT)`
(verified on device).

### MIDI on the Retroid ROM (device prep, RG DS)

> Verified on the RG DS (Retroid ROM V1.15, Android 14) **and** an
> Anbernic RK3568 (Android 14 engineering ROM) — same OS-level behaviour.

The Retroid ROM has the MIDI framework classes but `SystemServer` only
starts the MIDI system service when
`hasSystemFeature("android.software.midi")` — the ROM ships without
that feature, so `MidiManager` was unusable. Fix (root, persistent in
`/system`):

    adb remount
    adb push midi_feature.xml /system/etc/permissions/midi_feature.xml
    adb shell chcon u:object_r:system_file:s0 /system/etc/permissions/midi_feature.xml
    adb reboot

`midi_feature.xml`:

    <?xml version="1.0" encoding="utf-8"?>
    <permissions>
        <feature name="android.hardware.midi" />
        <feature name="android.software.midi" />
    </permissions>

After reboot: `service list | grep midi` → `midi:
[android.media.midi.IMidiManager]`, `dumpsys midi` works. Note the
service name is `midi` (not `media.midi`). The RG DS has **zero**
system MIDI ports (no USB/BT MIDI hardware) — device MIDI testing uses
the backend's in-process virtual loopback (`LibpdServer.midi_create_loopback()`).

### OEM-ROM enumeration hardening

Chinese-OEM Android 14 ROMs (e.g. the Anbernic "eng.builde" build) can
throw `NoSuchMethodError` from the hidden `ActivityThread.getApplication()`
call used to reach `MidiManager`. The backend's `jni_enumerate_devices()`
clears any pending JNI exception on **every** failure path and degrades to
"no system devices" (the loopback stays available) instead of letting the
exception abort the process on the next Godot `step()` JNI call.

### Debug APK signing

`gradlew assembleStandardDebug` alone produces an **unsigned** APK on a
fresh template tree. Sign before `adb install`:

    apksigner sign --ks ~/.android/debug.keystore --ks-key-alias androiddebugkey \
        --ks-pass pass:android --out android_debug.apk android_debug.apk

After rebuilding the extension `.so`, copy it to
`test_project/android/build/libs/debug/arm64-v8a/libgodot_libpd.so`
before `gradlew assembleStandardDebug`, otherwise the APK keeps the stale
`.so` (the Godot `--export-debug` step refreshes it too).

### Hotplug (M4) — live device add/remove on Android

The router re-enumerates the backend's `list_ports()` every
`midi_port_poll_interval` seconds (default 0.5) and fires
`midi_port_added` / `midi_port_removed` on the main thread, keyed by
**(direction, name)** — not index. On Android each re-enumeration is a fresh
`MidiManager.getDevices()` JNI query (via RtMidi's AMIDI `getPortCount()`/`getPortName()`),
so a plugged/unplugged USB-MIDI device is picked up on the next tick.
Removal is debounced (two consecutive empty enumerations); open real-device
ports whose device disappeared are auto-closed with
`midi_port_error(port, "device removed")`. **No auto-reopen** — the app
re-opens by re-querying `midi_list_inputs()`/`midi_list_outputs()` after a
`midi_port_added`.

**Device-free vehicle: the in-process loopback.** Android has no AMIDI
virtual-port API (`midi_open_virtual_input()` returns -1), so the
device-free self-test uses the backend's in-process loopback
(`midi_create_loopback(name)`, device indices 200 in / 201 out), which
enumerates in `list_ports()` while active → fires `midi_port_added`. The
loopback has **no runtime close**, so on-device it verifies the *added*
path only; the *removed* path is ctest-covered and exercised by a real
USB-MIDI unplug.

**Visibility is logcat-only.** GDScript `print()` reaches Android logcat; C
`printf` (e.g. the router's `[MIDI] backend=<name>` startup line) does
**not**. So the hotplug self-test's `HOTPLUG_SMOKE_*` lines appear in logcat
but the C-level backend banner does not.

Self-test: temp-switch the Android preset `command_line/extra_args` to
`"--headless --midi-hotplug-smoke"` and the project `run/main_scene` to
`res://scenes/test_midi.tscn`, re-export + sign + install, launch, then
`adb logcat -d | grep HOTPLUG` → `HOTPLUG_SMOKE_OK added=1 removed=0
(loopback ...)`.


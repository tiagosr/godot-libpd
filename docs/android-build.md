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
  `-ljvm` — the extension supplies a `JNI_GetCreatedJavaVMs` shim that
  dlsyms the real function from `libart.so` (NDK r25 ships no
  `libjvm.so`).

### MIDI on the Retroid ROM (device prep, RG DS)

The Retroid ROM (V1.15, Android 14) has the MIDI framework classes but
`SystemServer` only starts the MIDI system service when
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
the backend's in-process virtual loopback.


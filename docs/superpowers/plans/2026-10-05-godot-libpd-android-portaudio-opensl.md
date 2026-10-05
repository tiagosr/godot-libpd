# M6' — Android Native Audio via PortAudio (OpenSL ES)

Status: **DRAFT** (2026-10-05)
Supersedes: `docs/superpowers/plans/2026-10-05-godot-libpd-aaudio-android.md` (M6, closed as BLOCKED)
Spec: `docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md`

## Context / ruling

M6 recon (see `.superpowers/sdd/2026-10-05-godot-libpd-aaudio-android/progress.md`)
concluded that **AAudio is silently broken on the Anbernic RG DS** (Android 14,
RK3568): streams are mixed by audio_flinger to the Speaker but never reach the
codec output; HAL-direct (`tinyplay`) is also silent; the OEM framework-gated
`spk switch` mixer control cannot be set from app code. OpenSL ES, by contrast,
is **user-verified working** on this device (Godot's own Android driver and
PortAudio-OpenSL both produce sound).

**User ruling (2026-10-05):** close M6 (AAudio) as blocked; Android does NOT
stay on the a1 Godot-generator sink — it moves to the **native PortAudio path**,
synced with macOS / Linux / Knulli. AAudio→PortAudio contribution remains
explicitly deferred, out of scope.

## Goal

`NativeAudio` (M5 mix-down architecture) works on Android using
**PortAudio's OpenSL ES hostapi** as the audio backend — identical behavior and
code path to macOS (CoreAudio) and Linux (ALSA).

## Recon findings (done)

- `extension/CMakeLists.txt` already builds the vendored (pd-bundled)
  PortAudio in place via `add_library(portaudio_lib STATIC …)` with a
  per-platform hostapi branch; `NATIVE_AUDIO` is currently `OFF` on Android.
- The pd-bundled PortAudio tree (`extension/thirdparty/libpd/pure-data/
  portaudio/portaudio`) contains `hostapi/{alsa,asio,coreaudio,wasapi,wmme}`
  only — **no `hostapi/opensl`** (pd stripped it). API is 1.9.x
  (`Pa_Initialize/Terminate/OpenStream` present).
- NDK r25 sysroot **has** `libopenSLES.so` (aarch64, API 29+).
- NDK r25 and every installed SDK platform (30/32/34/35) **have no ASL
  headers** (`sLES.h`, `openSL.h`, `SLES_*.h`) → must be vendored.
- `PortAudioPort` and `NativeAudio` are fully `AudioPort`-agnostic; no
  architecture changes needed. Only the hostapi layer + build glue is new.

## Non-goals

- AAudio (blocked on device; revisit only with a working OEM build)
- Audio capture / `adc~` (separate milestone)
- Device enumeration (default output only)
- iOS / Windows
- Lowering minSdk (stays 29)

## Design

- **New thirdparty tree** `extension/thirdparty/portaudio-opensl/`
  (mirrors the `portmidi` / `rtmidi` vendor pattern):
  - `hostapi/` — upstream PortAudio 1.9.x `src/hostapi/opensl/` sources
    (`pa_android_opensl.c`, `sl_output.c`, `sl_input.c`,
    `sl_buffer_queue_util.{c,h}`, `asl_version.h`), pinned version recorded.
  - `asl/` — the ASL headers (`ASL/sLES.h`, `ASL/openSL.h`,
    `ASL/open_max.h`, `ASL/SLES_*.h`), from AOSP
    `platform/frameworks/native/opengl/include/ASL`, pinned to the release
    matching Android 14; provenance + license (Apache-2.0) recorded in a
    `README.md`.
- **CMake** (`extension/CMakeLists.txt`):
  - `if(ANDROID) → set(NATIVE_AUDIO ON)`.
  - `portaudio_lib` gains the OpenSL hostapi sources, `PA_USE_OPENSL=1`,
    include dirs for the vendored ASL tree, and links `openSLES`.
  - macOS/Linux branches untouched.
- **AudioPort / NativeAudio / mix-down path**: unchanged.
- **Stream params**: 48 kHz or device default (let PortAudio negotiate;
  log the negotiated rate — the RG DS is RK3568, expect 48k), stereo float,
  blocksize 256 (multiple of 64), same latency target as M5.
- **Failure behavior (decision)**: if the PortAudio stream fails to open on
  Android, `NativeAudio.open()` reports the error and the server falls back to
  the a1 `AudioStreamGenerator` sink for that instance, with a visible warning
  (graceful degradation on OEM hardware). macOS/Linux behavior unchanged.

## Tasks

### T1 — Vendor OpenSL hostapi + ASL headers
- Fetch PortAudio 1.9.x `src/hostapi/opensl/` (pin exact tag/commit) into
  `extension/thirdparty/portaudio-opensl/hostapi/`.
- Fetch the AOSP ASL headers into `extension/thirdparty/portaudio-opensl/asl/`.
- `README.md`: provenance, versions, licenses; `LICENSE` note.
- Gate: files present + `asl_version.h` resolves `SL_VERSION`/`SL_IENGINE_ID`
  for API ≥ 29.

### T2 — CMake Android branch
- `if(ANDROID)`: `NATIVE_AUDIO ON`, OpenSL hostapi sources + defines +
  `openSLES` link.
- Gate: cross-compiled `libgodot_libpd.so` (arm64-v8a) links cleanly against
  `libopenSLES.so`; no PortAudio symbol collisions; macOS/Linux builds still
  pass unchanged.

### T3 — Device spike (BLOCKING GATE)
- Standalone NDK binary (like the AAudio spike) using the vendored
  PortAudio+OpenSL: 440 Hz stereo sine, ~8 s, log negotiated
  rate/format/buffer + callback frame counts.
- Run on RG DS (`56cda52938004166`).
- Gate: **user hears the tone**. If silent → stop; M6' closes blocked,
  Android reverts to a1, and we reassess (possible OpenSL-hostapi-specific
  OEM issue).

### T4 — Test app on-device verify
- Build the APK (standard debug), deploy to RG DS.
- `test_native_mix` scene: multi-synth (8 synths + mix-down), user-confirmed
  audible output, no crackle, MIDI still working.
- ≥ 30 s soak; check logcat for warnings.
- Gate: audio verified by user; host test suite still green.

### T5 — Docs, ledger, cleanup, commit
- Spec §5 addendum: M6 (AAudio) closed blocked; M6' = PortAudio/OpenSL.
- `docs/android-build.md`: native audio note (OpenSL, no extra SDK deps).
- Progress ledger for M6'; revert any temp test switches; commit.

## Risks

1. **OpenSL hostapi staleness on Android 14** — it predates AAudio;
   mitigated by T3 gating with a minimal spike before any extension work.
2. **ASL header vs `libopenSLES.so` version skew** — pin headers to the
   Android 14-generation AOSP release; `asl_version.h` selects `SL_IID_IENGINE`
   (API ≥ 23) which matches the sysroot lib.
3. **Vendored PA os/unix layer on bionic** — same files PortAudio itself
   builds for Android; low risk, exposed by T2/T3.
4. **OEM quirks** — the device already proved OpenSL audibility with
   Godot/PortAudio, so the main residual risk is *our* build, not the device.

## Verification rules

- HARD VERIFICATION RULE for all worker reports (build logs, symbol dumps,
  device logs, and user audible-confirmation quoted verbatim).
- Host test suite (all binaries) green before T4.
- Devices left clean; no stray adb sessions.

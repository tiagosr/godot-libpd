# M6' — Android Native Audio via `OpenSLESPort` (OpenSL ES)

Status: **ACTIVE** (2026-10-05)
Supersedes: `docs/superpowers/plans/2026-10-05-godot-libpd-aaudio-android.md` (M6, closed as BLOCKED)
Spec: `docs/superpowers/specs/2026-10-03-godot-libpd-native-audio-design.md`

## Context / rulings

- M6 (AAudio) closed **BLOCKED** on the RG DS: AAudio streams are mixed by
  audio_flinger to the Speaker but never reach the codec; HAL-direct is also
  silent; OEM `spk switch` is framework-gated. OpenSL ES is **user-verified
  working** on this device.
- **Ruling 1:** Android does NOT stay on the a1 Godot-generator sink — it
  moves to the native mix-down path, synced with macOS / Linux / Knulli.
- **Ruling 2 (2026-10-05, plan revision):** original premise
  "vendor upstream PortAudio OpenSL hostapi" is **invalid** — upstream
  PortAudio has *never* shipped an OpenSL hostapi (verified across full git
  history, v19.0→v19.7/master; `src/hostapi/` = alsa/asio/coreaudio/dsound/
  jack/wasapi/wmme; CMake has no Android support).
- **Ruling 3:** **Option A** — implement `OpenSLESPort` directly on the OpenSL
  ES C API, as a second `AudioPort` implementation (the M6 `AAudioPort`
  design, with OpenSL instead of AAudio). No new audio-library dependency.
  Oboe rejected (added dependency + buffer-negotiation quirks).

## Goal

`NativeAudio` (M5 mix-down architecture) works on Android with
`OpenSLESPort : AudioPort` as the backend — identical behavior to macOS
(CoreAudio) / Linux (ALSA) via `PortAudioPort`.

## Architecture (no changes needed)

- `NativeAudio`, `MixInputRing`, synth workers: unchanged (`AudioPort`-
  agnostic; render callback contract: caller-allocated zero-initialized
  `frames * n_out` floats, fixed `blocksize` per callback).
- `LibpdServer` owns the `AudioPort*`; currently `#ifdef NATIVE_AUDIO`
  creates `PortAudioPort` at 2–3 sites → replaced by a
  **`create_platform_port()` factory** (core/): `PortAudioPort` on
  host, `OpenSLESPort` on `__ANDROID__`.
- `NATIVE_AUDIO` becomes **ON on Android** too (it currently gates the
  whole mix-down path + `PortAudioPort`). PortAudio is NOT built on
  Android; `OpenSLESPort` links `openSLES` instead.
- **Failure behavior (existing, free):** if the OpenSL stream fails to open,
  `audio_open()` returns false → `audio_open_` stays false → instances keep
  the a1 generator sinks. No new fallback code.

## New files

- `extension/src/core/opensles_port.{h,cpp}` — `OpenSLESPort : AudioPort`.
- `extension/src/core/platform_port_factory.{h,cpp}` — `create_platform_port()`.
- `extension/thirdparty/opensl-asl/ASL/*.h` — OpenSL ES API headers
  (AOSP `frameworks/native` @ android14-release, `opengl/include/ASL/`),
  + `README.md` (provenance, license Apache-2.0).
- `spike/opensl/opensles_spike.c` — device gate spike (scratch, gitignored
  build output).

## OpenSL ES design notes

- Objects: `SL_IID_ENGINE` → `SL_IID_PLAYBACK` → `SL_IID_BUFFERQUEUE` +
  `SL_IID_DATASink`; `SLDataCallback` is the audio-thread body (RT rules
  apply: no allocation/I/O/mutex/sleep — same as the AAudio design).
- Format: `SLDATASAMPLE_FORMAT_FLOAT` / `SL_PCMSAMPLEFORMAT_FIXED_32`,
  endianness `SL_BYTEORDER_BIGENDIAN` is NOT used — float is byte-order
  agnostic per the header; channels `SL_CHANNEL_MASK_FRONT_LEFT|RIGHT`.
- Fixed callback size: enqueue buffers of exactly `blocksize` frames; the
  buffer-queue callback always returns exactly the enqueued frame count →
  the `NativeAudio::render_block` fixed-gather contract holds.
- Sample rate: request 48000; if the implementation returns a different
  negotiated rate, honor it (log it) — the RG DS is RK3568, expect 48k.
  `NativeAudio`/server already take the samplerate as a parameter.
- `supports_input()` = false; `n_ins` must be 0 (output-only, same as the
  AAudio ruling). Device enumeration: non-goal → `list_outputs()` returns a
  single synthetic "OpenSL ES default output" row; `list_inputs()` empty.
- Latency: estimate from `SLPlayIt_Temporal` latency query where possible,
  else `buffer_count * blocksize / rate * 1000` (best-effort).
- Stream usage: `AUDIO_STREAM_MUSIC` (the path the user verified works).

## Tasks

### T1 — Vendor ASL headers
- Sparse/partial clone AOSP `platform/frameworks/native` @
  `android14-release`, take `opengl/include/ASL/` into
  `extension/thirdparty/opensl-asl/ASL/` + README (provenance, license,
  why: NDK r25 and installed SDK platforms 30/32/34/35 all lack ASL
  headers; `libopenSLES.so` IS in the NDK r25 sysroot).
- Gate: headers compile in a throwaway NDK translation unit
  (`#include <ASL/sLES.h>`, reference `SL_IID_ENGINE`).

### T2 — OpenSL ES device spike (BLOCKING GATE)
- `spike/opensl/opensles_spike.c`: engine + playback + bufferqueue,
  440 Hz stereo float sine, ~8 s, enqueue 256-frame buffers, log:
  negotiated rate / buffer size / callback counts + frame counts,
  clean `SLObject_Destroy` teardown. Rate configurable via argv
  (default 48000).
- Build: NDK r25 `aarch64-linux-android29-clang -I thirdparty ASL -lopenSLES`.
- Run on RG DS (`56cda52938004166`).
- Gate: **user hears the tone** AND callback frame counts are a fixed
  256 (contract check). If silent → M6' closes blocked, Android stays on a1,
  reassess (OpenSL-specific OEM issue or user observation was Godot-only).

### T3 — `OpenSLESPort` implementation
- `opensles_port.{h,cpp}` per the design notes; RAII-safe teardown;
  idempotent `close()`; RT-clean callback (zero-alloc, zero-syscall).
- Host-gateable surface: the OpenSL calls can't run on host — verification
  is via T4 build + T5 device (documented in review briefs).

### T4 — Platform port factory + CMake
- `platform_port_factory`: `create_platform_port()` (`PortAudioPort`
  host / `OpenSLESPort` Android); update the 2–3 `PortAudioPort`
  construction sites in `libpd_server.cpp`.
- CMake: `NATIVE_AUDIO ON` on Android; `opensles_port.cpp` +
  `platform_port_factory.cpp` in the extension + test targets;
  `-lopenSLES` on Android; PortAudio NOT built on Android (drop the
  `if(NOT ANDROID)` gating accordingly; `portaudio_port.cpp` stays
  host-only).
- Gate: arm64-v8a `libgodot_libpd.so` cross-compiles + links;
  macOS/Linux builds and the full host test suite still green.

### T5 — Test app on-device verify
- APK (standard debug), deploy to RG DS, `test_native_mix` scene:
  8 synths + mix-down, user-confirmed audible, no crackle, MIDI
  still works; ≥ 30 s soak; logcat clean (no OpenSL errors).
- Gate: user confirms audio + host suite green.

### T6 — Docs, ledger, cleanup, commit
- Spec §5 addendum: M6 closed blocked (AAudio evidence summary);
  M6' = OpenSLESPort.
- `docs/android-build.md`: native audio via OpenSL ES (no extra SDK
  components; ASL headers vendored).
- Progress ledger; revert temp switches; commit.

## Risks

1. **OpenSL buffer-queue callback granularity** — some HALs coalesce
   callbacks; the fixed-256 contract check in T2 de-risks this up front.
2. **ASL header vs `libopenSLES.so` skew** — headers pinned to
   android14-release (the device's API generation); the ASL ABI is
   frozen/stable.
3. **The user's OpenSL observation might have been Godot-only** — T2
   re-verifies with our own code path before any extension work.
4. **RT hygiene** — the OpenSL callback runs on the OpenSL audio thread;
   same rules as the PortAudio callback (kick → wait_done → gather).
   Proven by M5; the port just has to honor fixed frame counts.

## Verification rules

- HARD VERIFICATION RULE for all worker reports (build logs, symbol
  dumps, device logs, user audible-confirmation quoted verbatim).
- Host test suite (all binaries) green before T5.
- Devices left clean; no stray adb sessions.

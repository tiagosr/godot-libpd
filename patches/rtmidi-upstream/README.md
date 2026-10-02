# RtMidi upstream patchset

Four fixes for the **Android (`ANDROID_AMIDI`) backend** and one build-system
option, extracted from the `godot-libpd` vendored RtMidi submodule as a
git-applyable patchset for pull requests to
<https://github.com/thestk/rtmidi>.

| Patch | File(s) | Fix |
|-------|---------|-----|
| `0001-*.patch` | `RtMidi.cpp` | Android `MidiInAndroid::pollMidi` copies the message **by value**, discarding every multi-chunk sysex continuation; other backends reference it. Also raises the Android receive buffer 128 → 512 bytes. |
| `0002-*.patch` | `RtMidi.cpp` | `androidGetThreadEnv` called `JNI_GetCreatedJavaVMs` **directly**, which fails the `--no-undefined` Android link (no linkable `libart.so` in the NDK). Switches to a runtime `dlsym`. |
| `0003-*.patch` | `RtMidi.cpp` | `androidGetThreadEnv` resolves the `JavaVM*` via `dlsym(RTLD_DEFAULT)` and **caches** it (`std::call_once`) — a direct call / per-call `dlsym` fails in an app's linker namespace. *(One branch here is host-specific — see "Upstream-bound scope" below.)* |
| `0004-*.patch` | `CMakeLists.txt` | ALSA hotplug regression test is now guarded by `option(RTMIDI_BUILD_HOTPLUG_TEST … ON)` so embedders who vendor RtMidi as a static library can skip building/running it. |

Base: upstream `23b8cd5` ("Merge pull request #393"), which is master at
extraction time. All four patches apply cleanly with `git am` (verified:
`git am` on a clean `23b8cd5` worktree reproduces the vendored submodule
tree byte-for-byte).

## Apply

```sh
git clone https://github.com/thestk/rtmidi
cd rtmidi
git am 0001-*.patch 0002-*.patch 0003-*.patch 0004-*.patch   # (from this directory)
cmake -S . -B build -DRTMIDI_API=android -DANDROID=ON && cmake --build build
```

(Use `git am --edit` to adjust commit subjects if desired.)

## Upstream-bound scope (read this before submitting)

These four local commits were written to fit **how `godot-libpd` embeds
RtMidi** — as a static library inside a Godot GDExtension `.so`, on
Android. Three of the four are clean, standalone upstream fixes; **one
branch of patch 0003 is project-specific and must be removed** for the
upstream PR:

- **0001 (sysex accumulation)** — pure upstream bug fix. No project
  coupling. Submit as-is.
- **0002 + 0003 (JVM resolution)** — the *upstream-acceptable* fix is the
  combined behavior of the two: replace the direct `JNI_GetCreatedJavaVMs`
  call with a **cached** `dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")`
  lookup. Patch 0003 as committed also tries a host-exported symbol
  `gdpd_rtmidi_host_java_vm()` *first*:

  ```cpp
  auto host = (JavaVM* (*)()) dlsym(nullptr, "gdpd_rtmidi_host_java_vm");
  if (host != nullptr) { jvm = host(); if (jvm != nullptr) return; }
  ```

  That symbol is exported by the `godot-libpd` extension, **not** by
  RtMidi. For upstream, delete that `host` block and keep only the
  `dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")` path (the fallback the
  block already falls through to). The rest of 0003 (caching,
  `#include <mutex>`/`<dlfcn.h>`, `AttachCurrentThreadAsDaemon`) is
  project-agnostic.
- **0004 (hotplug option)** — pure build-system QoL, backward compatible
  (defaults `ON`). Submit as-is.

Why the host hook exists at all: on Android 14 an app's linker namespace
cannot `dlopen("libart.so")` by path and the `JavaVM*` is not in the
`RTLD_DEFAULT` scope of a GDExtension `.so`, so `godot-libpd` resolves it
itself (a `/proc/self/mem` ELF scan of the already-mapped `libart`) and
exposes it to RtMidi through that one `extern "C"` hook. That is a
property of *this* host, not of RtMidi — hence strip it upstream.

## Regression / verification

Unlike the PortMIDI patchset, there is **no host-side unit test shipped
here**: the sysex and JVM-resolution code paths live in the Android
`pollMidi` loop and the AMidi runtime, which only run on-device / in an
Android emulator with a (virtual) MIDI device. They are not reachable from
a macOS/Linux host build.

Verification instead:

- **Sysex (0001):** a full `F0..F7` message sent in a single chunk
  round-trips on an Android 14 arm64 device through the in-process
  virtual loopback (`MIDI_SMOKE_OK`, `midi_sysex` signal observed). The
  read-stage re-slicing of a whole-message sysex into `<=4`-byte raw words
  is separately covered by the host `midi_read_stage_tests`.
- **JVM resolution (0002/0003):** the extension's Android backend
  (which calls `androidGetThreadEnv` indirectly via RtMidi's Android
  in/out API) enumerates + opens a device on the same Android 14 device;
  `gdpd_rtmidi_host_java_vm()` supplies the `JavaVM*` and RtMidi's
  `AttachCurrentThreadAsDaemon` path is exercised. Clean exit, zero
  crashes.
- **Hotplug guard (0004):** `cmake -DRTMIDI_BUILD_HOTPLUG_TEST=OFF` builds
  the ALSA backend without compiling/running the hotplug test; the
  default (`ON`) is unchanged, so an upstream build is unaffected.

## Files / provenance

- Extracted via `git format-patch 23b8cd5..748eb75` from the `godot-libpd`
  vendored submodule (`extension/thirdparty/rtmidi`), commits:
  `759d4e6` (0001), `9727ab6` (0002), `b8b2720` (0003), `748eb75` (0004).
- The vendor keeps the same fixes in its submodule history independently;
  this package is the upstream-bound version. When upstream lands the
  fixes, the submodule can re-sync to upstream and drop commits 0001–0004
  (keeping only the project-specific host-hook branch of 0003, which
  would then be re-applied on top of the upstream JVM resolver).

# Draft PR text for thestk/rtmidi

The four vendored commits map to **three** upstream PRs. (0001, 0004 are
clean as-is; 0002+0003 collapse into one JVM-resolution PR after removing
the host-specific `gdpd_rtmidi_host_java_vm` branch — see README "Upstream-bound
scope".)

---

## PR 1 — Android: fix multi-chunk sysex in `MidiInAndroid::pollMidi`

### Title

`ANDROID_AMIDI: reference (not copy) the input message so multi-chunk sysex survives`

### Suggested body

```
MidiInAndroid::pollMidi currently does:

    if (numMessagesReceived > 0 && numBytesReceived >= 0) {
      auto message = self->inputData_.message;   // <-- copy by value

Every other in-API (CoreMidi, JACK, ALSA, OSS, Linux) uses a reference:

    MidiInApi::MidiMessage &message = ...

The copy is a bug for sysex. Android delivers a sysex message across
several pollMidi iterations: the first iteration receives the F0..F7 run
and sets continueSysex; later iterations append the continuation bytes
into inputData_.message. Because the local message is a COPY made at the
top of each iteration, the appended continuation bytes are read from a
stale copy and only the final chunk reaches the user callback — a
>128-byte or multi-iteration sysex is truncated / dropped.

Reference the message instead of copying it:

    MidiInApi::MidiMessage &message = self->inputData_.message;

This matches the other backends and lets sysex accumulate in
inputData_.message across iterations.

Also raises MAX_BYTES_TO_RECEIVE 128 -> 512 so a single large sysex chunk
is not split mid-run by the AMidi buffer (the Android buffer is a stack
array; 512 stays comfortably within thread stack limits).

Verified on an Android 14 arm64 device: a full F0..F7 sysex message now
round-trips intact through an in/out virtual pair (previously the
continuation chunks were lost).
```

### Patch
`0001-fix-ANDROID_AMIDI-accumulate-multi-chunk-sysex-acros.patch`

---

## PR 2 — Android: resolve the JVM without a direct `JNI_GetCreatedJavaVMs` call

### Title

`ANDROID_AMIDI: resolve the JavaVM via dlsym(RTLD_DEFAULT) and cache it`

### Suggested body

```
androidGetThreadEnv() currently calls JNI_GetCreatedJavaVMs(...)
directly, on every call:

    jint result = JNI_GetCreatedJavaVMs(&pjvms, 1, &jvmsFound);

Two problems:

1. The NDK does not ship a linkable libart.so, so a direct reference to
   JNI_GetCreatedJavaVMs leaves it undefined at link time for an
   Android shared library built with --no-undefined (the default for
   shared libs). dlopen("libart.so") by path is also unreliable: on
   Android 14 an app's linker namespace refuses to dlopen libart.

2. The JVM is resolved on every call (no caching). There is exactly one
   JVM per process, so resolve it once.

Fix: resolve the function pointer at runtime with
dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs") — the symbol is present in
the process (libart is always loaded) and reachable from the default
scope — and cache the JavaVM* with std::call_once:

    static JavaVM* jvm = nullptr;
    static std::once_flag once;
    std::call_once(once, []() {
      auto fn = (jint (*)(JavaVM**, jsize, jsize*))
          dlsym(nullptr, "JNI_GetCreatedJavaVMs");
      ...
      jvm = jvms;
    });

Add #include <dlfcn.h> and <mutex> to the Android section. The rest of
the function (GetEnv / AttachCurrentThreadAsDaemon) is unchanged.

Verified on an Android 14 arm64 device: the AMidi in/out backends
enumerate and open a device and deliver events; the previous direct call
fails the shared-library link.
```

### Patch
Combined from `0002-*.patch` + `0003-*.patch`, **minus** the
`gdpd_rtmidi_host_java_vm` host-hook block (remove it; keep the
`dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")` fallback path and the
caching).

---

## PR 3 — build: opt-out of the ALSA hotplug regression test

### Title

`build: guard the ALSA hotplug test behind RTMIDI_BUILD_HOTPLUG_TEST`

### Suggested body

```
The ALSA hotplug regression test is compiled and run whenever the ALSA
backend is built, unconditionally:

    if("alsa" IN_LIST API_LIST)
      include(tests/AlsaHotplug.cmake)
      rtmidi_add_hotplug_test(rtmidi ...)

Embedders who vendor RtMidi as a static library (e.g. inside a larger
shared library / engine) often don't want the hotplug test built or run
in their environment. Guard it behind an option that defaults to ON so
the standard build is unchanged:

    option(RTMIDI_BUILD_HOTPLUG_TEST
      "Build and run the ALSA hotplug regression test." ON)
    if("alsa" IN_LIST API_LIST AND RTMIDI_BUILD_HOTPLUG_TEST)

Backward compatible: default ON keeps the existing behavior.
```

### Patch
`0004-Guard-the-ALSA-hotplug-regression-test-behind-RTMIDI.patch`

---

## Notes for the submitter

- **0001 and 0004** apply verbatim. **0002+0003** should be squashed into a
  single JVM-resolution commit and the `gdpd_rtmidi_host_java_vm` branch
  deleted (it belongs to the `godot-libpd` host, not to RtMidi).
- No host-side unit test is possible for the Android paths (they only run
  against the AMidi runtime); verification was on-device (Android 14
  arm64) — see README "Regression / verification".
- Commit subjects can be cleaned with `git am --edit` before pushing to a
  fork.
- The `godot-libpd` project keeps these fixes vendored in
  `extension/thirdparty/rtmidi`; if/when upstream lands them, the vendor
  submodule re-syncs to upstream and re-applies only the project-specific
  host-hook branch on top.

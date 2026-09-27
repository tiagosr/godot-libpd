# godot-libpd GDExtension

Multi-instance, worker-threaded libpd (Pure Data) GDExtension for Godot 4.6,
with Godot-native audio (AudioStreamGenerator). See
`../docs/superpowers/specs/2026-09-27-godot-libpd-gdextension-design.md`.

## Pinned third-party versions

| Submodule | Pin |
|---|---|
| `thirdparty/godot-cpp` | branch `4.5`, commit `27d9dd2` (godot-4.5-stable-30-g27d9dd2) |
| `thirdparty/libpd` | `ba0dc63` (libpd 0.16.1, pd vanilla 0.56-5; pure-data submodule @ f009fd8) |
| `thirdparty/portmidi` | `6b51c25` (compiled in, wiring is v2) |

## Building

```
./build.sh --macos          # extension/build/macos/libgodot_libpd.dylib
./build.sh --linux-arm64    # extension/build/linux/libgodot_libpd.so  (inside Knulli Docker)
./build.sh --android        # extension/build/android-arm64/libgodot_libpd.so (NDK in $NDK)
```

Note: CMake >= 4.x may require `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
depending on godot-cpp's scripts (tracked here if needed).

## Testing

Headless tests live in `../test_project` (main scene switched per test;
see the implementation plan). Unit tests: `ctest --test-dir build/cmake-macos`.

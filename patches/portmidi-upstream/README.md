# PortMIDI upstream patchset

Two fixes for `Pm_OpenInput` / ALSA virtual ports, extracted from the
`godot-libpd` vendored PortMIDI submodule as a git-applyable patchset
for a pull request to <https://github.com/PortMidi/PortMidi>.

| Patch | File | Fix |
|-------|------|-----|
| `0001-*.patch` | `pm_common/portmidi.c` | `Pm_OpenInput` missing bounds check (crash/hang on out-of-range device index) |
| `0002-*.patch` | `pm_linux/pmlinuxalsa.c` | ALSA virtual ports stamped with a pre-allocation `timestamp_queue` never deliver events |

Base: upstream `6b51c25` ("Merge pull request #107"), which is current
master at extraction time. Both patches apply cleanly with `git am`.

## Apply

```sh
git clone https://github.com/PortMidi/PortMidi
cd PortMidi
git am 0001-*.patch 0002-*.patch   # (from this directory)
cmake -S . -B build && cmake --build build
```

(Use `git am --edit` to adjust commit subjects if desired.)

## Regression test

`test_pm_open_bounds.c` — backend-agnostic C test for the
`Pm_OpenInput` contract (out-of-range index ⇒ clean
`pmInvalidDeviceId`, no UB; valid index still opens). Build:

```sh
clang test_pm_open_bounds.c -o test_pm_open_bounds \
    -I <portmidi>/pm_common -I <portmidi>/porttime \
    -L <portmidi-build> -lportmidi
# macOS additionally: -framework CoreMIDI -framework CoreAudio -framework CoreFoundation
# ALSA build additionally: -lportmidi links alsa
./test_pm_open_bounds   # expect: PASS: open-bounds regression (N devices); exit 0
```

Verified on macOS (CoreMIDI backend): passes against the patched
build.

## Why patch 0001 (the important one)

`Pm_OpenOutput` validates its index (`outputDevice < 0 ||
outputDevice >= pm_descriptor_len`), but `Pm_OpenInput` indexes
`pm_descriptors[inputDevice]` unconditionally:

```c
if (!pm_descriptors[inputDevice].pub.input)      /* portmidi.c:958 (6b51c25) */
    err = pmInvalidDeviceId;
```

`pm_descriptors` is allocated in 32-slot growth chunks
(`pm_descriptor_max += 32`), so two distinct hazards exist:

1. **index == `pm_descriptor_len`** (the first "out of range" index):
   reads a *within-allocation but uninitialized* descriptor record.
   The result is heap-garbage-dependent: `pub.input` non-zero makes
   the (invalid) device *pass* validation and proceed into
   `pm_create_internal()` and the backend open with a bogus device id.
   On a Trimui Brick (A133; kernel 4.9.191, ALSA sequencer with **zero**
   SUBS-capable input ports, so `pm_descriptor_len == 0` and the app
   called `Pm_OpenInput(NULL, 0, ...)` after validating its own index
   against a stale device count) this corrupted process state and the
   calling thread's next synchronous wait hung the whole application.
2. **index > allocation**: a straight heap out-of-bounds read.

Deterministic demonstration on stock macOS (vanilla upstream, 2
visible devices):

```
$ Pm_OpenInput(&s, Pm_CountDevices(), ...)    # index == len == 2
==pid==ERROR: AddressSanitizer: BUS on unknown address
SUMMARY: AddressSanitizer: BUS portmidi.c:958 in Pm_OpenInput
```

i.e. a SIGBUS in `Pm_OpenInput` on a perfectly ordinary desktop host.
The patched build returns `pmInvalidDeviceId` for both `index == len`
and `index == 10000` and continues running (exit 0).

The fix mirrors the existing `Pm_OpenOutput` guard:

```c
if (inputDevice < 0 || inputDevice >= pm_descriptor_len) {
    *stream = NULL;
    return pmInvalidDeviceId;
}
```

## Why patch 0002 (ALSA)

`alsa_create_virtual()` stamped every newly created ALSA virtual port
with the client's `timestamp_queue` id **unconditionally** — including
when the sequencer client was created *before* any queue existed,
leaving the field holding the unallocated client's `queue_used` value
(0 on first creation). On an ALSA sequencer without queue 0 available
(fresh Trimui Brick: queue 0 exists but is owned/stopped; the app's
client then got queue 1), events to the stamped queue were queued into
a queue the receiver never read → **virtual ports delivered zero
events**, breaking `aconnect`-based loopback while direct
`Pm_WriteShort` → open-input appeared fine.

The fix stamps `timestamp_queue` only when the client actually has an
allocated queue (`queue_used > 0`); otherwise the kernel default
(real-time delivery) applies. Verified on-device (A133): with the
guard, `aconnect out→in` delivers events (loopback tone + note events
observed); without it, nothing is delivered.

## Files / provenance

- Extracted via `git format-patch 6b51c25..6be63b7` from the
  `godot-libpd` vendored submodule
  (`extension/thirdparty/portmidi`), commits `883b32f` (0001) and
  `6be63b7` (0002).
- The vendor keeps the same fixes in its submodule history
  independently; this package is the upstream-bound version.

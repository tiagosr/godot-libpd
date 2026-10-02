# Draft PR text for PortMidi/PortMidi

## Title

`Pm_OpenInput: add missing device index bounds check (crash/hang on out-of-range index)`

(Second patch can be a follow-up PR: `pm_linux: only stamp virtual ports with a timestamp queue that exists`)

## Suggested body

```
Pm_OpenOutput validates its device index, but Pm_OpenInput indexes
pm_descriptors[inputDevice] unconditionally (portmidi.c):

    if (!pm_descriptors[inputDevice].pub.input)
        err = pmInvalidDeviceId;

pm_descriptors is allocated in 32-slot growth chunks, so the missing
check has two failure modes:

1. inputDevice == pm_descriptor_len: reads a within-allocation but
   UNINITIALIZED descriptor record. The garbage pub.input field can
   be non-zero, in which case the invalid device PASSES validation and
   processing continues into pm_create_internal() and the backend open
   with a bogus device id.

2. inputDevice above the allocation: straight heap out-of-bounds read.

Deterministic repro on stock macOS (vanilla upstream 6b51c25, 2 visible
devices, ASan):

    PmError e = Pm_OpenInput(&s, Pm_CountDevices(), NULL, 16, NULL, NULL);
    /* index == 2 == count */

    ==pid==ERROR: AddressSanitizer: BUS on unknown address
    SUMMARY: AddressSanitizer: BUS portmidi.c:958 in Pm_OpenInput

Field report (ALSA backend, Trimui Brick A133, kernel 4.9.191): the
sequencer exposes zero SUBS-capable input ports (count == 0); an
application calling Pm_OpenInput(NULL, 0, ...) after a device-count
check against a stale count corrupted process state and the calling
thread hung permanently on its next synchronization.

This patch mirrors the existing Pm_OpenOutput guard:

    if (inputDevice < 0 || inputDevice >= pm_descriptor_len) {
        *stream = NULL;
        return pmInvalidDeviceId;
    }

Also included: a small backend-agnostic regression test
(test_pm_open_bounds.c) pinning the contract:
  - Pm_OpenInput(index == Pm_CountDevices()) -> pmInvalidDeviceId,
    *stream left NULL
  - Pm_OpenInput(10000) -> pmInvalidDeviceId (pre-patch: heap OOB)
  - Pm_OpenOutput(index == Pm_CountDevices()) -> pmInvalidDeviceId
    (already guarded; pinned for symmetry)
  - when an input device exists, opening it still succeeds

Verified: passes against the patched build on macOS (CoreMIDI backend).
The ALSA guard path is exercised by the same contract on any ALSA host
(the error path is backend-independent).
```

## Notes for the submitter

- The ALSA `timestamp_queue` fix (patch 0002) is a separate concern —
  recommend a **second PR** with its own explanation (see README.md).
  It was needed for `aconnect`-based loopback on the A133; on hosts
  where queue 0 is available it is a no-op.
- Commit subjects can be cleaned up with `git am --edit` before pushing
  to your fork.
- The `godot-libpd` project keeps the same fixes vendored in
  `extension/thirdparty/portmidi`; if/when upstream lands these, the
  vendor submodule can re-sync to upstream and drop the local commits.

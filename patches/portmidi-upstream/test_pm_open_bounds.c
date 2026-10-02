/*
 * test_pm_open_bounds.c — regression test for the Pm_OpenInput bounds check
 * (patch 0001 in this directory).
 *
 * Upstream PortMIDI's Pm_OpenOutput validates its device index
 * (`outputDevice < 0 || outputDevice >= pm_descriptor_len`) but
 * Pm_OpenInput indexes pm_descriptors[inputDevice] unconditionally.
 * pm_descriptors is allocated in 32-slot growth chunks, so two hazards
 * exist: index == count reads an *uninitialized* (within-allocation)
 * descriptor record whose garbage can make the invalid device pass
 * validation (observed: A133 app hang on an ALSA host with zero
 * SUBS-capable input ports); index > allocation is a straight heap
 * OOB read (observed: SIGBUS at portmidi.c:958 on stock macOS under
 * ASan with index == count).
 *
 * This test is backend-agnostic: it only requires that opening an
 * out-of-range index returns pmInvalidDeviceId *without* UB, on any
 * host, regardless of how many devices exist.
 *
 * Build (any platform, against a PortMidi build of your choice):
 *   clang test_pm_open_bounds.c -o test_pm_open_bounds \
 *       -I<path-to-portmidi>/pm_common -I<path-to-portmidi>/porttime \
 *       <portmidi-static-lib> [-framework CoreMIDI -framework CoreFoundation]
 * Exit code 0 = pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include "portmidi.h"

int main(void) {
	int failures = 0;

	if (Pm_Initialize() != pmNoError) {
		printf("FAIL: Pm_Initialize\n");
		return 1;
	}

	/* The out-of-range index is the live device count: the index space is
	 * 0..Pm_CountDevices()-1, so `count` itself is the first
	 * guaranteed-invalid id (input and output share one index space; a
	 * device may be input-only, output-only, or both). */
	const int count = Pm_CountDevices();

	PortMidiStream *stream = (PortMidiStream *)0xdeadbeef;
	PmError err;

	/* 1) Pm_OpenInput out of range must be a clean pmInvalidDeviceId.
	 * Pre-patch, this was an out-of-bounds heap read when count == 0
	 * (and garbage-dependent behavior in general). */
	err = Pm_OpenInput(&stream, count, NULL, 16, NULL, NULL);
	if (err != pmInvalidDeviceId) {
		printf("FAIL: Pm_OpenInput(index=%d) = %d, expected pmInvalidDeviceId\n",
		       count, (int)err);
		failures++;
	}
	if (stream != NULL) {
		printf("FAIL: Pm_OpenInput left *stream non-NULL on error\n");
		failures++;
	}

	/* 2) Pm_OpenOutput out of range (already guarded upstream — pinned
	 * here so the two paths stay symmetric). */
	err = Pm_OpenOutput(&stream, count, NULL, 16, NULL, NULL, 0);
	if (err != pmInvalidDeviceId) {
		printf("FAIL: Pm_OpenOutput(index=%d) = %d, expected pmInvalidDeviceId\n",
		       count, (int)err);
		failures++;
	}
	if (stream != NULL) {
		printf("FAIL: Pm_OpenOutput left *stream non-NULL on error\n");
		failures++;
	}

	/* 3) Far out of range: with the guard this is a clean error; without
	 * it, a straight heap out-of-bounds read (demonstrated as a SIGBUS
	 * in Pm_OpenInput on stock macOS under ASan). */
	err = Pm_OpenInput(&stream, 10000, NULL, 16, NULL, NULL);
	if (err != pmInvalidDeviceId) {
		printf("FAIL: Pm_OpenInput(10000) = %d, expected pmInvalidDeviceId\n", (int)err);
		failures++;
	}

	/* 4) Sanity: when an input device DOES exist, opening it must
	 * still work (the guard must not over-reject). Skipped on hosts
	 * with no input devices — the contract under test is the error
	 * path, and a zero-device host is exactly where the pre-patch
	 * OOB read lived. */
	{
		int first_input = -1;
		for (int i = 0; i < count; i++) {
			const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
			if (info && info->input) {
				first_input = i;
				break;
			}
		}
		if (first_input >= 0) {
			PortMidiStream *s2 = NULL;
			err = Pm_OpenInput(&s2, first_input, NULL, 16, NULL, NULL);
			if (err != pmNoError || s2 == NULL) {
				printf("FAIL: Pm_OpenInput(%d) with input device present = %d\n",
				       first_input, (int)err);
				failures++;
			} else {
				Pm_Close(s2);
			}
		}
	}

	Pm_Terminate();

	if (failures == 0) {
		printf("PASS: open-bounds regression (%d devices)\n", count);
		return 0;
	}
	printf("FAIL: %d check(s) failed\n", failures);
	return 1;
}

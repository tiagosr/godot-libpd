// PortMIDI virtual device lifecycle test (Task 7): create -> open ->
// write (output) -> close -> Pm_DeleteVirtualDevice, no error at any
// step. Delivery is NOT verified here: an in-process loopback would
// need a concurrent Pm_Read on the matching virtual input, which this
// harness does not do — the A133 on-device run (aconnect loopback,
// recipe in docs/knulli-build.md) is the delivery proof.
//
// Uses a test-only device name (the app's own ports use "libpd test
// app in 0" / "libpd test app out 0") so a running app can never
// collide with this test.

#include <cstdio>

#include <portmidi.h>

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                                \
	} while (0)

int main() {
	if (Pm_Initialize() != pmNoError) {
		std::printf("FAIL Pm_Initialize\n");
		return 1;
	}

	// Create the virtual output device (CoreMIDI destination on macOS).
	const int out_id =
			Pm_CreateVirtualOutput("gdlibpd pmtest out", nullptr, nullptr);
	CHECK(out_id >= 0); // negative -> pmInvalidDeviceId / pmNameConflict
	if (out_id < 0) {
		std::printf("FAIL Pm_CreateVirtualOutput -> %s\n",
				Pm_GetErrorText(static_cast<PmError>(out_id)));
		Pm_Terminate();
		return 1;
	}
	// The new device is listed with an output side.
	{
		const PmDeviceInfo *info = Pm_GetDeviceInfo(out_id);
		CHECK(info != nullptr);
		if (info != nullptr) {
			CHECK(info->output != 0);
		}
	}

	// Open it through the same call the router uses (latency 0).
	PortMidiStream *stream = nullptr;
	const PmError open_err =
			Pm_OpenOutput(&stream, out_id, nullptr, 256, nullptr, nullptr, 0);
	CHECK(open_err == pmNoError);
	if (open_err != pmNoError) {
		std::printf("FAIL Pm_OpenOutput -> %s\n", Pm_GetErrorText(open_err));
		Pm_DeleteVirtualDevice(out_id);
		Pm_Terminate();
		return 1;
	}

	// Write the full-form note-on word the router's output path emits.
	CHECK(Pm_WriteShort(stream, 0, Pm_Message(0x90, 60, 100)) == pmNoError);
	CHECK(Pm_HasHostError(stream) == 0);

	// Close, then delete the now-closed device.
	CHECK(Pm_Close(stream) == pmNoError);
	CHECK(Pm_DeleteVirtualDevice(out_id) == pmNoError);
	// A deleted device no longer resolves.
	CHECK(Pm_GetDeviceInfo(out_id) == nullptr);

	Pm_Terminate();
	if (failures != 0) {
		std::printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("midi_virtual_port_tests OK\n");
	return 0;
}

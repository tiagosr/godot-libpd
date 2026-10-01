// A133 freeze regression: MidiRouter open_input/open_output with an
// out-of-range PM index must fail FAST with the port_error signal —
// never hang, never read the vendored pm_descriptors table out of
// bounds.
//
// The repro: the A133's ALSA sequencer enumerates zero PM devices
// (pm_descriptor_len == 0), so opening "index 0" indexed
// pm_descriptors[0] in the vendored Pm_OpenInput, which — unlike
// Pm_OpenOutput — had no bounds check: OOB heap read (UB), the I/O
// thread died, the open was never signaled, and the app froze.
//
// Headless, no device needed: any PM host works, because the check
// is against Pm_CountDevices() — on a host with N devices the first
// out-of-range index is N.
//
// Watchdog: each risky call (open / shutdown) runs under a deadline
// thread that kills this process if the call outlives it — a hang
// regression fails ctest (non-zero exit) instead of hanging it. The
// deadline is far above the router's own 500 ms control budget, so a
// correctly fixed build never approaches it.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "midi_router.h"

using godot_libpd::MidiRouter;

static int failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                 \
		}                                                                \
	} while (0)

namespace {

constexpr int kWatchdogMs = 10000; // vs the router's 500 ms op budget

// Kills the process if p_what outlives kWatchdogMs. Disarmed (and the
// thread joined) when the guarded call returns in time.
class Watchdog {
public:
	explicit Watchdog(const char *p_what) {
		deadline_ =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(kWatchdogMs);
		thread_ = std::thread([this, p_what] {
			while (std::chrono::steady_clock::now() < deadline_) {
				if (!armed_.load()) {
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (armed_.load()) {
				std::printf("WATCHDOG TIMEOUT: %s outlived %d ms "
						"(hang/OOB regression)\n",
						p_what, kWatchdogMs);
				std::fflush(stdout);
				std::_Exit(2); // a hung open must fail ctest, not hang it
			}
		});
	}

	~Watchdog() {
		armed_.store(false);
		if (thread_.joinable()) {
			thread_.join();
		}
	}

private:
	std::atomic<bool> armed_{true};
	std::chrono::steady_clock::time_point deadline_;
	std::thread thread_;
};

} // namespace

int main() {
	MidiRouter router;
	if (!router.available()) {
		std::printf("FAIL Pm_Initialize (router unavailable)\n");
		return 1;
	}
	// The I/O thread polls Pm_* only for open streams, and no stream is
	// open yet, so this direct enumeration (same call the router
	// validates against) does not race a concurrent PM use.
	const int device_count = Pm_CountDevices();

	std::mutex errors_mutex;
	std::vector<std::string> errors;
	router.on_port_error = [&](int p_port_id, const char *p_what) {
		std::lock_guard<std::mutex> lock(errors_mutex);
		(void)p_port_id;
		errors.push_back(p_what != nullptr ? p_what : "");
	};

	// First out-of-range index for both open flavors.
	{
		Watchdog wd("open_input(out-of-range)");
		CHECK(router.open_input(device_count) == -1);
	}
	{
		Watchdog wd("open_output(out-of-range)");
		CHECK(router.open_output(device_count) == -1);
	}
	// Negative index (the other side of the validated range).
	{
		Watchdog wd("open_input(-1)");
		CHECK(router.open_input(-1) == -1);
	}

	CHECK(errors.size() >= 3);
	for (const std::string &e : errors) {
		// The open-failure pattern: pmInvalidDeviceId text (see the
		// vendored errmsg table: "PortMidi: Invalid device ID").
		CHECK(e.find("Invalid device ID") != std::string::npos);
	}
	if (!errors.empty()) {
		std::printf("first error: %s\n", errors[0].c_str());
	}

	{
		Watchdog wd("shutdown");
		router.shutdown();
	}
	if (failures != 0) {
		std::printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("midi_open_bounds_tests OK (device_count=%d)\n", device_count);
	return 0;
}

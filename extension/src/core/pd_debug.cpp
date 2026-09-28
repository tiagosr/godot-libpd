#include "pd_debug.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace godot_libpd {

namespace {
std::atomic<bool> g_checked{false};
bool g_enabled = false;
const char *g_dir = "/tmp";

std::chrono::steady_clock::time_point g_anchor;
std::once_flag g_anchor_once;
}

bool pd_dbg_enabled() {
	if (!g_checked.load(std::memory_order_relaxed)) {
		const char *v = std::getenv("PD_DBG");
		g_enabled = (v != nullptr && v[0] != '\0');
		const char *d = std::getenv("PD_DBG_DIR");
		if (d != nullptr && d[0] != '\0') {
			g_dir = d;
		}
		g_checked.store(true, std::memory_order_relaxed);
	}
	return g_enabled;
}

double pd_dbg_elapsed_ms() {
	std::call_once(g_anchor_once, [] {
		g_anchor = std::chrono::steady_clock::now();
	});
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_anchor).count();
}

void pd_dbg_log(uint32_t p_instance_id, const char *p_msg) {
	if (!pd_dbg_enabled()) {
		return;
	}
	char path[512];
	std::snprintf(path, sizeof(path), "%s/godot_libpd_%u.log", g_dir, p_instance_id);
	FILE *f = std::fopen(path, "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "%s\n", p_msg);
	std::fclose(f);
}

} // namespace godot_libpd

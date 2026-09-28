#pragma once

#include <cstdint>

namespace godot_libpd {

/**
 * Device-debug logging for hang diagnosis (Trimui Brick A133).
 *
 * Enabled by setting the PD_DBG environment variable to any value.
 * Lines are appended to $PD_DBG_DIR/godot_libpd_<id>.log (default dir /tmp,
 * which is tmpfs on the device — writes survive SIGKILL, unlike exfat
 * /userdata where the file size is only committed at close()).
 *
 * Call sites: worker thread lifecycle/commands/teardown (libpd_worker.cpp),
 * main-thread waits and joins (libpd_instance.cpp). All lines in a given
 * instance file carry the same global time base, so worker and main-thread
 * events interleave into one timeline.
 */
void pd_dbg_log(uint32_t p_instance_id, const char *p_msg);

/** True when PD_DBG is set (checked once at first use). */
bool pd_dbg_enabled();

/** Milliseconds since the first logging call (shared anchor, all threads). */
double pd_dbg_elapsed_ms();

} // namespace godot_libpd

/**
 * One-time registration of the vendored pd externals (cyclone + else;
 * see cmake/pdexternals.cmake). Must run after libpd_init() and before
 * any patch load; called from LibpdWorker::init_pd_globals_once().
 *
 * class_new() registers into the global class list and into the method
 * tables of every existing instance (class_doaddmethod iterates
 * pd_ninstances); instances created later inherit the classes via
 * pdinstance_new(). So a single process-level call covers all
 * instances.
 */
#pragma once

namespace godot_libpd {
void register_pdexternals();
} // namespace godot_libpd

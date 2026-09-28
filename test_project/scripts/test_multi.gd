extends Node2D
# Task 6: Multi-instance stress + teardown guarantees.
#  - 4 concurrent instances, all rendering + printing independently
#  - spawn/kill cycles: free instances WHILE dsp is active (join in _exit_tree)
#  - server unregistration + memory-delta soft check

const PATCH := "res://data/test_patch.pd"

# Poll a condition each frame until true or timeout; returns whether it held.
func _wait_until(cond: Callable, timeout_s: float) -> bool:
	var deadline := Time.get_ticks_msec() + int(timeout_s * 1000.0)
	while Time.get_ticks_msec() < deadline:
		if cond.call():
			return true
		await get_tree().process_frame
	return cond.call()

func _ready() -> void:
	var srate := int(AudioServer.get_mix_rate())
	var mem_before := OS.get_static_memory_usage()

	# --- 4 concurrent instances -------------------------------------------
	var instances := []
	for i in 4:
		var inst := LibpdInstance.new()
		add_child(inst)
		assert(inst.init(srate), "init %d failed" % i)
		assert(inst.load_patch(PATCH) == Error.OK, "load %d failed" % i)
		assert(inst.start_dsp() == Error.OK, "start_dsp %d failed" % i)
		instances.append(inst)

	var prints := {}
	Libpd.server.instance_print.connect(func(id, text):
		if "test_patch" in text:
			prints[id] = prints.get(id, 0) + 1)

	# Wait until every instance is actively pushing rendered blocks.
	var ids: Array = []
	for i in instances:
		ids.append(i.instance_id)
	var ready := func() -> bool:
		for i in instances:
			if not i.dsp_active or i.debug_blocks_pushed <= 0:
				return false
		return true
	assert(await _wait_until(ready, 5.0), "instances not all running within timeout")

	# --- all 4 print independently on a note ------------------------------
	for i in instances:
		i.send_midi(0, 60, 100)
	var all_printed := func() -> bool:
		for idv in ids:
			if prints.get(idv, 0) < 1:
				return false
		return true
	assert(await _wait_until(all_printed, 5.0), "expected prints from all 4, got %s" % str(prints))

	# --- spawn/kill cycles: freed WHILE dsp is active ---------------------
	for r in 3:
		var extra := LibpdInstance.new()
		add_child(extra)
		assert(extra.init(srate) and extra.load_patch(PATCH) == Error.OK and extra.start_dsp() == Error.OK,
				"cycle %d setup failed" % r)
		var extra_id: int = extra.instance_id
		await get_tree().create_timer(0.3).timeout
		extra.queue_free() # while dsp_active: must stop + join cleanly
		await get_tree().process_frame
		await get_tree().process_frame
		assert(not Libpd.server.instance_registered(extra_id),
				"cycle %d: instance %d still registered after free" % [r, extra_id])

	var mem_after := OS.get_static_memory_usage()
	var delta_mb := (mem_after - mem_before) / 1048576.0
	print("mem delta over 3 spawn/kill cycles: %.1f MB" % delta_mb)
	assert(delta_mb < 64.0, "memory growth too large: %.1f MB" % delta_mb)

	# --- teardown of the 4 long-lived instances ---------------------------
	for i in instances:
		var id: int = i.instance_id
		i.stop_dsp()
		i.queue_free()
		await get_tree().process_frame
		assert(not Libpd.server.instance_registered(id), "instance %d still registered" % id)

	print("TEST6_OK prints=%s" % str(prints))
	get_tree().quit(0)

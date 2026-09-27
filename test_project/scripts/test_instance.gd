extends Node2D
# Task 4: LibpdInstance + worker thread (real pd, dry sink).

func _ready() -> void:
	var inst := LibpdInstance.new()
	add_child(inst)
	assert(inst.init(44100), "init failed")
	assert(inst.init(44100) == false, "double init must fail")
	assert(inst.load_patch("res://data/nope.pd") != Error.OK, "missing patch must fail")
	assert(inst.load_patch("res://data/test_patch.pd") == Error.OK, "load failed")
	assert(inst.patch_loaded)
	var got_note := []
	Libpd.server.instance_print.connect(func(id, text):
		if "test_patch" in text:
			got_note.append(text))
	assert(inst.start_dsp() == Error.OK)
	await get_tree().create_timer(0.3).timeout
	assert(inst.debug_blocks_pushed > 0, "worker dsp loop not pushing blocks")
	inst.send_midi(0, 60, 100) # main thread, dsp running
	await get_tree().create_timer(1.0).timeout
	assert(got_note.size() > 0, "note print not received: %s" % got_note)
	assert(inst.debug_sink_peak > 0.0, "no audio rendered (peak=0)")
	inst.send_midi(0, 60, 0) # note off -> gate closes
	assert(inst.stop_dsp() == Error.OK)
	assert(inst.unload_patch() == Error.OK)
	var final_peak := inst.debug_sink_peak
	var final_blocks := inst.debug_blocks_pushed
	inst.queue_free()
	await get_tree().process_frame
	print("TEST4_OK peak=%.4f blocks=%d" % [final_peak, final_blocks])
	get_tree().quit(0)

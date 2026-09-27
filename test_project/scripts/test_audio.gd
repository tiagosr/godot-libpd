extends Node2D
# Task 5: AudioStreamGenerator sink (Godot 4.6) + samplerate fail-fast.
# Uses the actual AudioServer mix rate for the normal path so the test is
# portable across platforms (headless/macOS=44100, Android often 48000).

func _ready() -> void:
	var srate := int(AudioServer.get_mix_rate())
	var inst := LibpdInstance.new()
	add_child(inst)
	assert(inst.init(srate), "init failed (mix rate %d)" % srate)
	assert(inst.load_patch("res://data/test_patch.pd") == Error.OK, "load failed")
	assert(inst.start_dsp() == Error.OK)
	await get_tree().create_timer(0.3).timeout
	assert(inst.debug_blocks_pushed > 0, "worker dsp loop not pushing blocks: %d" % inst.debug_blocks_pushed)

	# Open the patch's note gate so audio is actually rendered.
	inst.send_midi(0, 60, 100)
	await get_tree().create_timer(0.5).timeout
	assert(inst.debug_sink_peak > 0.0, "no audio rendered (peak=0) after note")

	# Samplerate fail-fast: mismatched rate must fail and emit failure.
	var bad_rate := 48000 if srate == 44100 else 44100
	var inst2 := LibpdInstance.new()
	add_child(inst2)
	var failures := []
	inst2.failure.connect(func(code, text): failures.append(text))
	assert(inst2.init(bad_rate) == false, "mismatched samplerate must fail fast")
	assert(failures.size() > 0, "failure signal not emitted for samplerate mismatch")

	inst.send_midi(0, 60, 0)
	inst.stop_dsp()
	var final_peak := inst.debug_sink_peak
	var final_blocks := inst.debug_blocks_pushed
	inst.queue_free()
	inst2.queue_free()
	await get_tree().process_frame
	print("TEST5_OK srate=%d peak=%.4f blocks=%d" % [srate, final_peak, final_blocks])
	get_tree().quit(0)

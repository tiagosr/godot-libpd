extends Node
# Temporary A/B tool (M6' T2): sustained 440 Hz tone through Godot's own
# audio stack (AudioStreamGenerator -> Godot's OpenSL driver on Android).
# Comparison target: spike/opensl/opensles_spike.c (direct OpenSL).
# Prints STARTED/STOPPED markers; quits itself.

var gen: AudioStreamGenerator
var gen_pb
var player: AudioStreamPlayer
var srate := 48000

func _ready() -> void:
	srate = int(AudioServer.get_mix_rate())
	print("AUDIO_AB srate=%d" % srate)
	print("AUDIO_AB STARTED (12 s tone)")
	gen = AudioStreamGenerator.new()
	gen.mix_rate = srate
	gen.buffer_length = 0.5
	player = AudioStreamPlayer.new()
	player.stream = gen
	add_child(player)
	player.play()
	await get_tree().process_frame
	gen_pb = player.get_stream_playback()
	if gen_pb == null:
		print("AUDIO_AB ERROR: get_stream_playback returned null")
		get_tree().quit(1)
		return
	print("AUDIO_AB playing=%s pb_playing=%s avail0=%d" % [player.is_playing(), gen_pb.is_playing(), int(gen_pb.get_frames_available())])
	var err := await _sustain(12.0)
	print("AUDIO_AB avail_end=%d skips=%s" % [int(gen_pb.get_frames_available()), str(gen_pb.get_skips())])
	print("AUDIO_AB STOPPED err=%d" % err)
	get_tree().quit(0)

func _sustain(duration: float) -> int:
	var start := Time.get_ticks_msec()
	var phase := 0.0
	var dphase := 2.0 * PI * 440.0 / srate
	var n := 512
	var total_pushed := 0
	while Time.get_ticks_msec() - start < int(duration * 1000.0):
		# PACE TO THE AUDIO CLOCK: push back-to-back until the buffer is
		# nearly full, then yield. Pushing one chunk per process_frame paces
		# to the render loop (~60 Hz) -> underruns -> skips -> growl.
		var pushed_any := false
		while int(gen_pb.get_frames_available()) >= n:
			var frames := []
			for i in range(n):
				var v := 0.5 * sin(phase)
				frames.append(Vector2(v, v))
				phase = fmod(phase + dphase, TAU)
				total_pushed += 1
			var ok: bool = gen_pb.push_buffer(frames)
			if not ok:
				return ERR_INVALID_DATA
			pushed_any = true
		if not pushed_any:
			await get_tree().process_frame
	return OK

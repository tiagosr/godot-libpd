extends Node
# M5 native audio acceptance (T8): 8 synth LibpdInstance workers + 1 mix-down
# instance, mixed through the PortAudio callback to the Mac's output device.
#
# GDScript 4.6 — `#` comments only (this project has no `//` comments).
# The `Libpd` autoload wraps the C++ LibpdServer (Libpd.server.<method>).
#
# How to run: open this scene in the editor and hit Play (▶). You will hear a
# C-major chord (8 different notes) mixed down to the Mac's output. Close the
# window to stop. (It runs until you close it — no auto-close.)

# A C-major-ish spread across the 8 synths so the mix is audibly a chord.
var synth_notes: Array = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 523.25, 587.33]
var instances: Array = []
var mixer: LibpdInstance = null
var started := false
var n_synth: int = 8

func _ready() -> void:
	var env: String = OS.get_environment("NM_SYNTHS")
	if env != "":
		var ns: int = int(env)
		if ns >= 0 and ns <= 8:
			n_synth = ns
	print("[native_mix] n_synth=%d" % n_synth)
	var blocksize: int = 256
	var envb: String = OS.get_environment("NM_BLOCK")
	if envb != "":
		var bs: int = int(envb)
		if bs > 0 and bs % 64 == 0:
			blocksize = bs
	print("[native_mix] blocksize=%d" % blocksize)
	print("[native_mix] ready")
	if not Libpd.server.audio_available():
		push_error("[native_mix] audio_available() == false; cannot run the native mix test")
		return
	if not Libpd.server.audio_open(blocksize, 44100):
		push_error("[native_mix] audio_open(%d, 44100) failed" % blocksize)
		return

	# The mix-down instance: role MIXER (control-only worker; rendered by the
	# dedicated mix render thread, drained by the PortAudio callback).
	mixer = LibpdInstance.new()
	mixer.set_role(1) # ROLE_MIXER
	add_child(mixer)
	var mix_in: int = 16
	var envin: String = OS.get_environment("NM_MIX_IN")
	if envin != "":
		mix_in = int(envin)
	print("[native_mix] mixer in_channels=%d" % mix_in)
	if not mixer.init(44100, mix_in, 2):
		push_error("[native_mix] mixer.init failed")
		return
	if mixer.load_patch("res://data/mixdown_16.pd") != Error.OK:
		push_error("[native_mix] mixer.load_patch(mixdown_16.pd) failed")
		return

	# 8 synth instances: role SYNTH (default), 0 in / 2 out, each into its own
	# ring; each plays a different note.
	for i in n_synth:
		var s: LibpdInstance = LibpdInstance.new()
		add_child(s)
		if not s.init(44100, 0, 2):
			push_error("[native_mix] synth %d init(44100, 0, 2) failed" % i)
			return
		if s.load_patch("res://data/synth.pd") != Error.OK:
			push_error("[native_mix] synth %d load_patch(synth.pd) failed" % i)
			return
		s.send_pd_message("note", [str(synth_notes[i])])
		instances.append(s)

	if not Libpd.server.set_mixer(mixer):
		push_error("[native_mix] set_mixer(mixer) failed")
		return
	Libpd.server.instance_print.connect(func(id, text):
		print("ID: ", id, " - ", text)
	)

	mixer.start_dsp()
	for s in instances:
		s.start_dsp()

	started = true
	print("[native_mix] PLAYING 8 mixed notes; output latency = %.2f ms" % Libpd.server.audio_output_latency_ms())
	print("[native_mix] listen for the chord; close the window to stop")

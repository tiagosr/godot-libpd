extends Node2D
# Task 3: LibpdServer signal fan-out (main thread) from the event ring.

var got := {}


func _ready() -> void:
	Libpd.server.instance_print.connect(func(id, text): got["print"] = [id, text])
	Libpd.server.instance_note_on.connect(func(id, ch, pitch, vel): got["note"] = [id, ch, pitch, vel])
	Libpd.server.instance_dsp_active.connect(func(id, active): got["active"] = [id, active])
	Libpd.server.debug_push_print(1, "hello pd")
	for i in 3:
		await get_tree().process_frame
	assert(got.has("print") and got["print"][0] == 1 and got["print"][1] == "hello pd", "print signal not delivered: %s" % got)
	print("TEST3_OK")
	get_tree().quit(0)

extends Node
# Abstraction-args test app: instantiate data/abstr.pd with index 42;
# the patch's $1-substituted receiver is track-42, which [send]s to
# host.out. Subscribe host.out, send a bang to track-42, assert the
# bang arrived. Prints ABSTRACTION_OK / ABSTRACTION_FAIL and quits.
#
# Run: Godot --headless --path test_project res://scenes/test_abstraction.tscn

const ABSTR := "res://data/abstr.pd"
const TIMEOUT_MS := 2000

var _failures := 0
var _got_bang := 0
var _start_ms := 0
var _instance: LibpdInstance = null


func _ready() -> void:
	_instance = LibpdInstance.new()
	add_child(_instance)
	if not _instance.init(44100):
		_finish("ABSTRACTION_FAIL init")
		return
	Libpd.server.instance_bang.connect(_on_instance_bang)
	if _instance.subscribe_receiver("host.out") != 0:
		_fail("subscribe host.out")
	if _instance.load_abstraction(ABSTR, ["42"]) != Error.OK:
		_fail("load_abstraction")
	_instance.send_pd_message("track-42", [])
	_start_ms = Time.get_ticks_msec()


func _process(_delta: float) -> void:
	if _start_ms == 0:
		return
	if Time.get_ticks_msec() - _start_ms >= TIMEOUT_MS:
		if _got_bang != 1:
			_fail("bang (got %d)" % _got_bang)
		_finish()


func _on_instance_bang(_id: int, recv: String) -> void:
	if recv == "host.out":
		_got_bang += 1


func _finish(result := "") -> void:
	if result == "":
		result = "ABSTRACTION_OK" if _failures == 0 else "ABSTRACTION_FAIL"
	print(result, " (failures=%d)" % _failures)
	get_tree().quit(0 if _failures == 0 else 1)


func _fail(what: String) -> void:
	_failures += 1
	print("FAIL: ", what)

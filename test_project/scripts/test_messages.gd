extends Node
# Receive-hook test app (M7): host-side subscription over libpd_bind.
# Subscribes names, sends bang/float/symbol/list via send_pd_message,
# drives a real patch [send] path, asserts the four server signals
# fire with the right payloads, prints MSG_RECV_OK / MSG_RECV_FAIL
# and quits (0 / 1).
#
# Run: Godot --headless --path test_project res://scenes/test_messages.tscn

const PATCH := "res://data/msgrecv.pd"
const TIMEOUT_MS := 2000

var _failures := 0
var _got_bang := ""
var _got_float := -1.0
var _got_symbol := ""
var _got_list := []
var _got_list_typed := []
var _got_demo_bang := 0
var _start_ms := 0
var _instance: LibpdInstance = null


func _ready() -> void:
	_instance = LibpdInstance.new()
	add_child(_instance)
	if not _instance.init(44100):
		_finish("MSG_RECV_FAIL init")
		return
	if _instance.load_patch(PATCH) != Error.OK:
		_finish("MSG_RECV_FAIL load")
		return
	if _instance.subscribe_receiver("gd.bang") != 0:
		_fail("subscribe gd.bang")
	if _instance.subscribe_receiver("gd.float") != 0:
		_fail("subscribe gd.float")
	if _instance.subscribe_receiver("gd.symbol") != 0:
		_fail("subscribe gd.symbol")
	if _instance.subscribe_receiver("gd.list") != 0:
		_fail("subscribe gd.list")
	if _instance.subscribe_receiver("gd.demo.bang") != 0:
		_fail("subscribe gd.demo.bang")
	if _instance.subscribe_receiver("gd.bang") != -1:
		_fail("duplicate subscribe must fail")
	Libpd.server.instance_bang.connect(_on_instance_bang)
	Libpd.server.instance_float.connect(_on_instance_float)
	Libpd.server.instance_symbol.connect(_on_instance_symbol)
	Libpd.server.instance_list.connect(_on_instance_list)
	Libpd.server.instance_list_typed.connect(_on_instance_list_typed)
	_instance.send_pd_message("gd.bang", [])
	_instance.send_pd_message("gd.float", ["0.618"])
	_instance.send_pd_message("gd.symbol", ["attack"])
	_instance.send_pd_message("gd.list", ["1.5", "attack", "2.0"])
	_instance.send_pd_message("trig", [])
	_start_ms = Time.get_ticks_msec()


func _process(_delta: float) -> void:
	if _start_ms == 0:
		return
	if Time.get_ticks_msec() - _start_ms >= TIMEOUT_MS:
		_asserts()
		_finish()


func _on_instance_bang(_id: int, recv: String) -> void:
	if recv == "gd.demo.bang":
		_got_demo_bang += 1
	elif recv == "gd.bang":
		_got_bang = recv


func _on_instance_float(_id: int, _recv: String, v: float) -> void:
	_got_float = v


func _on_instance_symbol(_id: int, _recv: String, s: String) -> void:
	_got_symbol = s


func _on_instance_list(_id: int, _recv: String, items: Array) -> void:
	_got_list = items


func _on_instance_list_typed(_id: int, _recv: String, items: Array) -> void:
	_got_list_typed = items


func _asserts() -> void:
	if _got_bang != "gd.bang":
		_fail("bang (got %s)" % _got_bang)
	if absf(_got_float - 0.618) > 1e-6:
		_fail("float (got %f)" % _got_float)
	if _got_symbol != "attack":
		_fail("symbol (got %s)" % _got_symbol)
	if _got_list.size() != 3:
		_fail("list size (got %d)" % _got_list.size())
	elif _got_list[0] != 1.5 or not typeof(_got_list[0]) == TYPE_FLOAT:
		_fail("list[0] (got %s)" % str(_got_list[0]))
	elif _got_list[1] != "attack" or not typeof(_got_list[1]) == TYPE_STRING:
		_fail("list[1] (got %s)" % str(_got_list[1]))
	elif _got_list[2] != 2.0 or not typeof(_got_list[2]) == TYPE_FLOAT:
		_fail("list[2] (got %s)" % str(_got_list[2]))
	# Explicit (type, value) tuples on the second path.
	if _got_list_typed.size() != 3:
		_fail("list_typed size (got %d)" % _got_list_typed.size())
	else:
		for i in range(3):
			var item: Array = _got_list_typed[i]
			if item.size() != 2:
				_fail("list_typed[%d] not a tuple (got %s)" % [i, str(item)])
				break
		if _got_list_typed.size() == 3:
			var t0: Array = _got_list_typed[0]
			var t1: Array = _got_list_typed[1]
			var t2: Array = _got_list_typed[2]
			if t0[0] != "float" or t0[1] != 1.5:
				_fail("list_typed[0] (got %s)" % str(t0))
			if t1[0] != "symbol" or t1[1] != "attack":
				_fail("list_typed[1] (got %s)" % str(t1))
			if t2[0] != "float" or t2[1] != 2.0:
				_fail("list_typed[2] (got %s)" % str(t2))
	if _got_demo_bang != 1:
		_fail("patch [send] bang (got %d)" % _got_demo_bang)


func _finish(result := "") -> void:
	if result == "":
		result = "MSG_RECV_OK" if _failures == 0 else "MSG_RECV_FAIL"
	print(result, " (failures=%d)" % _failures)
	get_tree().quit(0 if _failures == 0 else 1)


func _fail(what: String) -> void:
	_failures += 1
	print("FAIL: ", what)

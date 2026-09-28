extends Control
## Task 7: full test app UI (spec §9) + a `--smoke` headless self-test mode.
##
## UI: title/samplerate label, instance list, Load Patch / Start-Stop DSP /
## Send Test Note / +Instance / Kill Last buttons, and a log of the last 20
## instance_print lines. The scene starts with one pre-created (not yet
## initialized) LibpdInstance.
##
## --smoke: skip the UI, run a headless self-test, and quit (0 on success,
## 1 on failure). Used by all platform verifications.

const PATCH := "res://data/test_patch.pd"
const MAX_LOG_LINES := 20

@onready var status_label: Label = $VBox/StatusLabel
@onready var instance_label: Label = $VBox/InstanceLabel
@onready var log: RichTextLabel = $VBox/Log
@onready var load_btn: Button = $VBox/Buttons/LoadPatchBtn
@onready var start_stop_btn: Button = $VBox/Buttons/StartStopBtn
@onready var note_btn: Button = $VBox/Buttons/SendNoteBtn
@onready var add_btn: Button = $VBox/Buttons/AddInstanceBtn
@onready var kill_btn: Button = $VBox/Buttons/KillLastBtn

var _instances: Array = [] # LibpdInstance nodes (scene-tree children)
var _inited := {}          # instance_id -> true
var _loaded := {}          # instance_id -> true
var _dsp_running := false
var _mix_rate := 44100

func _ready() -> void:
	_mix_rate = int(AudioServer.get_mix_rate())
	if "--smoke" in OS.get_cmdline_args():
		_run_smoke()
		return
	load_btn.pressed.connect(_on_load_pressed)
	start_stop_btn.pressed.connect(_on_start_stop_pressed)
	note_btn.pressed.connect(_on_note_pressed)
	add_btn.pressed.connect(_on_add_pressed)
	kill_btn.pressed.connect(_on_kill_pressed)
	Libpd.server.instance_print.connect(_on_instance_print)
	Libpd.server.instance_note_on.connect(_on_instance_note_on)
	Libpd.server.instance_dsp_active.connect(_on_instance_dsp_active)
	# One pre-created (not yet initialized) instance.
	_add_instance(false)
	_log("godot-libpd test app ready (mix rate %d)" % _mix_rate)
	_refresh_labels()

# --------------------------------------------------------------------------
# --smoke headless self-test
# --------------------------------------------------------------------------
func _run_smoke() -> void:
	# 1) create + init
	var inst := LibpdInstance.new()
	add_child(inst)
	if not inst.init(_mix_rate):
		print("SMOKE_FAIL init"); get_tree().quit(1); return
	# 2) load
	if inst.load_patch(PATCH) != Error.OK:
		print("SMOKE_FAIL load"); get_tree().quit(1); return
	# 3) start dsp, expect rendered blocks
	if inst.start_dsp() != Error.OK:
		print("SMOKE_FAIL start"); get_tree().quit(1); return
	await get_tree().create_timer(0.5).timeout
	if inst.debug_blocks_pushed <= 0:
		print("SMOKE_FAIL blocks"); get_tree().quit(1); return
	# 4) send a note, expect a print
	var got_print := []
	Libpd.server.instance_print.connect(func(id, text):
		if "test_patch" in text:
			got_print.append(1))
	inst.send_midi(0, 60, 100)
	await get_tree().create_timer(0.5).timeout
	if got_print.is_empty():
		print("SMOKE_FAIL print"); get_tree().quit(1); return
	# 5) spawn a second instance, run, free it
	var inst2 := LibpdInstance.new()
	add_child(inst2)
	if not inst2.init(_mix_rate) or inst2.load_patch(PATCH) != Error.OK or inst2.start_dsp() != Error.OK:
		print("SMOKE_FAIL second"); get_tree().quit(1); return
	await get_tree().create_timer(0.5).timeout
	inst2.stop_dsp()
	inst2.queue_free()
	await get_tree().process_frame
	# 6) stop + free first, success
	inst.stop_dsp()
	inst.queue_free()
	await get_tree().process_frame
	print("SMOKE_OK")
	get_tree().quit(0)

# --------------------------------------------------------------------------
# UI handlers
# --------------------------------------------------------------------------
func _add_instance(load_and_start: bool) -> void:
	var inst := LibpdInstance.new()
	add_child(inst)
	inst.failure.connect(_on_instance_failure.bind(inst))
	_instances.append(inst)
	if load_and_start:
		_ensure_ready(inst)
		inst.start_dsp()
	_refresh_labels()

func _on_load_pressed() -> void:
	for inst in _instances:
		_ensure_ready(inst)
	_log("load patch requested (%d instance(s))" % _instances.size())

func _ensure_ready(inst: LibpdInstance) -> void:
	var id: int = inst.instance_id
	if not _inited.has(id):
		if inst.init(_mix_rate):
			_inited[id] = true
		else:
			_log("init failed for instance %d" % id)
			return
	if not _loaded.has(id):
		if inst.load_patch(PATCH) == Error.OK:
			_loaded[id] = true
		else:
			_log("load failed for instance %d" % id)

func _on_start_stop_pressed() -> void:
	if _dsp_running:
		for inst in _instances:
			inst.stop_dsp()
		_dsp_running = false
	else:
		for inst in _instances:
			var id: int = inst.instance_id
			if _inited.has(id) and _loaded.has(id):
				inst.start_dsp()
		_dsp_running = true
	start_stop_btn.text = "Stop DSP" if _dsp_running else "Start DSP"
	_refresh_labels()

func _on_note_pressed() -> void:
	for inst in _instances:
		var id: int = inst.instance_id
		if _inited.has(id) and _loaded.has(id):
			inst.send_midi(0, 60, 100)
	_log("test note sent to %d instance(s)" % _instances.size())

func _on_add_pressed() -> void:
	_add_instance(true)

func _on_kill_pressed() -> void:
	if _instances.is_empty():
		return
	var inst: LibpdInstance = _instances.back()
	var id: int = inst.instance_id
	inst.stop_dsp()
	_instances.pop_back()
	_inited.erase(id)
	_loaded.erase(id)
	inst.queue_free()
	_refresh_labels()

# --------------------------------------------------------------------------
# LibpdServer signals
# --------------------------------------------------------------------------
func _on_instance_print(id: int, text: String) -> void:
	_log("print[%d]: %s" % [id, text])

func _on_instance_note_on(id: int, ch: int, pitch: int, vel: int) -> void:
	_log("note[%d]: ch=%d pitch=%d vel=%d" % [id, ch, pitch, vel])

func _on_instance_dsp_active(id: int, active: bool) -> void:
	_log("dsp[%d]: %s" % [id, "on" if active else "off"])

func _on_instance_failure(inst: LibpdInstance, code: int, text: String) -> void:
	_log("failure[%d]: code=%d %s" % [inst.instance_id, code, text])

# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
func _refresh_labels() -> void:
	status_label.text = "godot-libpd test app   (sample rate: %d)" % _mix_rate
	instance_label.text = "instances: %d   dsp: %s" % [_instances.size(), "RUNNING" if _dsp_running else "stopped"]

func _log(text: String) -> void:
	log.append_text(text + "\n")
	while log.get_line_count() > MAX_LOG_LINES:
		log.delete_line(0)
	if log.get_line_count() > 0:
		log.scroll_to_line(log.get_line_count() - 1)

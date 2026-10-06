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
const PATCH_EXTERNALS := "res://data/external_patch.pd" # cyclone + else objects
const MAX_LOG_LINES := 20

# --smoke diagnostics: a flushed progress file (survives a hard kill, unlike
# block-buffered stdout) + a wall-clock watchdog so a hang can never run long
# enough to overload the target device (e.g. the A133's kernel watchdog).
const SMOKE_PROGRESS := "/tmp/smoke_progress.log"
const SMOKE_TIMEOUT_MS := 15000

@onready var status_label: Label = $VBox/StatusLabel
@onready var instance_label: Label = $VBox/InstanceLabel
@onready var log: Label = $VBox/Log
@onready var load_btn: Button = $VBox/Buttons/LoadPatchBtn
@onready var start_stop_btn: Button = $VBox/Buttons/StartStopBtn
@onready var note_btn: Button = $VBox/Buttons/SendNoteBtn
@onready var add_btn: Button = $VBox/Buttons/AddInstanceBtn
@onready var kill_btn: Button = $VBox/Buttons/KillLastBtn

var _instances: Array = [] # LibpdInstance nodes (scene-tree children)
var _log_lines: Array = [] # ring buffer of the last MAX_LOG_LINES strings
var _inited := {}          # instance_id -> true
var _loaded := {}          # instance_id -> true
var _dsp_running := false
var _mix_rate := 44100

# --smoke diagnostics state.
var _smoke_file: FileAccess = null
var _smoke_start := 0
var _smoke_active := false
var _smoke_done := false

func _ready() -> void:
	_mix_rate = int(AudioServer.get_mix_rate())
	if "--smoke" in OS.get_cmdline_args():
		_smoke_active = true
		_smoke_start = Time.get_ticks_msec()
		_smoke_file = FileAccess.open(SMOKE_PROGRESS, FileAccess.WRITE)
		_smoke_log("START mix_rate=%d pid_args=%s" % [_mix_rate, str(OS.get_cmdline_args())])
		_run_smoke()
		return
	_ensure_ui_actions()
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
	# Initial GUI focus so D-pad navigation has a starting point.
	load_btn.grab_focus()
	_log("godot-libpd test app ready (mix rate %d)" % _mix_rate)
	_log("dpad moves focus - labeled B activates")
	_log("quit: labeled A or Start+Select")
	_refresh_labels()

func _ensure_ui_actions() -> void:
	# The test project ships no [input] map, so register the standard
	# ui_* navigation actions: arrow keys (what the evdev DS maps the
	# D-pad to) plus the joypad D-pad buttons the same events carry.
	# JoyButton: A=0 Back=4 DPAD_UP=11 DPAD_DOWN=12 DPAD_LEFT=13 DPAD_RIGHT=14.
	var nav := {
		"ui_left": [KEY_LEFT, 13],
		"ui_right": [KEY_RIGHT, 14],
		"ui_up": [KEY_UP, 11],
		"ui_down": [KEY_DOWN, 12],
	}
	for action: String in nav:
		if not InputMap.has_action(action):
			InputMap.add_action(action)
		var ke := InputEventKey.new()
		ke.physical_keycode = nav[action][0]
		InputMap.action_add_event(action, ke)
		var je := InputEventJoypadButton.new()
		je.button_index = nav[action][1]
		InputMap.action_add_event(action, je)
	var accept := [KEY_ENTER, KEY_SPACE, 0] # Enter, Space, joypad A
	for v: int in accept:
		if not InputMap.has_action("ui_accept"):
			InputMap.add_action("ui_accept")
		if v < 10000:
			var ke2 := InputEventKey.new()
			ke2.physical_keycode = v
			InputMap.action_add_event("ui_accept", ke2)
		else:
			var je2 := InputEventJoypadButton.new()
			je2.button_index = v
			InputMap.action_add_event("ui_accept", je2)
	if not InputMap.has_action("ui_cancel"):
		InputMap.add_action("ui_cancel")
	var ke3 := InputEventKey.new()
	ke3.physical_keycode = KEY_ESCAPE
	InputMap.action_add_event("ui_cancel", ke3)
	var je3 := InputEventJoypadButton.new()
	je3.button_index = 4 # Back
	InputMap.action_add_event("ui_cancel", je3)

func _input(event: InputEvent) -> void:
	# Evdev input echo (display-server level, pre-Control) so handheld
	# button activity is visible on-device. Raw codes: the physical
	# evdev code is in the DS-level mapping, the keycode/button index
	# here is the Godot-side result.
	if event is InputEventKey and event.pressed and not event.echo:
		var kmsg := "input: KEY code=%d phys=%d" % [event.keycode, event.physical_keycode]
		print(kmsg)
		_log(kmsg)
	elif event is InputEventJoypadButton and event.pressed:
		var jmsg := "input: JOY button=%d" % event.button_index
		print(jmsg)
		_log(jmsg)

func _unhandled_input(event: InputEvent) -> void:
	# ui_cancel (B/ESC, or joypad Back) quits so the frontend can always
	# take the display back; ui_accept (labeled B -> Enter, or joypad A)
	# activates the focused button.
	if event.is_action_pressed("ui_cancel"):
		_log("quitting (ui_cancel: B/ESC/Back)")
		get_tree().quit()
		accept_event()

# --------------------------------------------------------------------------
# --smoke headless self-test
# --------------------------------------------------------------------------
func _smoke_log(step: String) -> void:
	var msg := "SMOKE | %7d ms | %s" % [Time.get_ticks_msec() - _smoke_start, step]
	print(msg)
	if _smoke_file != null:
		_smoke_file.store_line(msg)
		_smoke_file.flush()

func _finish_smoke(code: int) -> void:
	_smoke_done = true
	if _smoke_file != null:
		_smoke_file.close()
		_smoke_file = null
	get_tree().quit(code)

func _process(_delta: float) -> void:
	# Wall-clock watchdog: independent of any single await, force-exit if the
	# smoke test has not finished in time (guards the target device from a
	# hung, CPU-spinning process). Only active in --smoke mode.
	if _smoke_active and not _smoke_done:
		if Time.get_ticks_msec() - _smoke_start > SMOKE_TIMEOUT_MS:
			_smoke_log("WATCHDOG_TIMEOUT quit(2)")
			_smoke_done = true
			get_tree().quit(2)

func _run_smoke() -> void:
	# 1) create + init
	_smoke_log("step1 new + init")
	var inst := LibpdInstance.new()
	add_child(inst)
	if not inst.init(_mix_rate):
		_smoke_log("SMOKE_FAIL init"); _finish_smoke(1); return
	_smoke_log("step1 init ok")
	# 2) load
	_smoke_log("step2 load_patch")
	if inst.load_patch(PATCH) != Error.OK:
		_smoke_log("SMOKE_FAIL load"); _finish_smoke(1); return
	_smoke_log("step2 load ok")
	# 2b) externals: the vendored cyclone + else objects must be
	#     registered in-process (else [op + 1] + cyclone [phasor~]).
	_smoke_log("step2b load external patch")
	if inst.load_patch(PATCH_EXTERNALS) != Error.OK:
		_smoke_log("SMOKE_FAIL externals load"); _finish_smoke(1); return
	_smoke_log("step2b externals load ok")
	# 2c) native audio: open the PortAudio mix stream AND bind a MIXER
	#     instance - the render callback only kicks the synth rings once
	#     a mixer is bound (same flow as test_native_mix), so without it
	#     the kick-driven SYNTH worker renders nothing.
	_smoke_log("step2c audio_open + mixer")
	if not Libpd.server.audio_available():
		_smoke_log("SMOKE_FAIL audio_available"); _finish_smoke(1); return
	if not Libpd.server.audio_open(256, _mix_rate):
		_smoke_log("SMOKE_FAIL audio_open"); _finish_smoke(1); return
	var mixer := LibpdInstance.new()
	add_child(mixer)
	mixer.set_role(1) # ROLE_MIXER
	if not mixer.init(_mix_rate, 16, 2):
		_smoke_log("SMOKE_FAIL mixer init"); _finish_smoke(1); return
	if mixer.load_patch("res://data/mixdown_16.pd") != Error.OK:
		_smoke_log("SMOKE_FAIL mixer load"); _finish_smoke(1); return
	if not Libpd.server.set_mixer(mixer):
		_smoke_log("SMOKE_FAIL set_mixer"); _finish_smoke(1); return
	mixer.start_dsp()
	_smoke_log("step2c mixer bound")
	# 3) start dsp, expect rendered blocks
	if inst.start_dsp() != Error.OK:
		_smoke_log("SMOKE_FAIL start"); _finish_smoke(1); return
	_smoke_log("step3 start_dsp ok, await 0.5s")
	await get_tree().create_timer(0.5).timeout
	_smoke_log("step3 woke, blocks_pushed=%d" % inst.debug_blocks_pushed)
	if inst.debug_blocks_pushed <= 0:
		_smoke_log("SMOKE_FAIL blocks"); _finish_smoke(1); return
	# 4) send a note, expect a print
	var got_print := []
	Libpd.server.instance_print.connect(func(id, text):
		if "test_patch" in text or "external_patch" in text:
			got_print.append(text))
	inst.send_midi(0, 60, 100)
	_smoke_log("step4 midi sent, await 0.5s")
	await get_tree().create_timer(0.5).timeout
	_smoke_log("step4 woke, got_print=%d" % got_print.size())
	if got_print.is_empty():
		_smoke_log("SMOKE_FAIL print"); _finish_smoke(1); return
	var got_external := got_print.any(func(t): return t.strip_edges().begins_with("external_patch") and t.ends_with("61"))
	if not got_external:
		_smoke_log("SMOKE_FAIL externals print: %s" % str(got_print)); _finish_smoke(1); return
	_smoke_log("step4 externals confirmed (else op: 60+1=61)")
	# 5) spawn a second instance, run, free it
	_smoke_log("step5 second instance new+init+load+start")
	var inst2 := LibpdInstance.new()
	add_child(inst2)
	if not inst2.init(_mix_rate) or inst2.load_patch(PATCH) != Error.OK or inst2.start_dsp() != Error.OK:
		_smoke_log("SMOKE_FAIL second"); _finish_smoke(1); return
	_smoke_log("step5 second ok, await 0.5s")
	await get_tree().create_timer(0.5).timeout
	_smoke_log("step5 woke, stopping+freeing inst2")
	inst2.stop_dsp()
	inst2.queue_free()
	await get_tree().process_frame
	_smoke_log("step5 inst2 freed (dtor+worker.join done)")
	# 6) stop + free first, free the mixer, success
	_smoke_log("step6 stopping+freeing first")
	inst.stop_dsp()
	inst.queue_free()
	mixer.stop_dsp()
	mixer.queue_free()
	await get_tree().process_frame
	_smoke_log("step6 first + mixer freed")
	Libpd.server.audio_close()
	print("SMOKE_OK")
	_smoke_log("SMOKE_OK calling quit(0)")
	_finish_smoke(0)

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
	# Plain Label log: keep a GDScript-side ring buffer and rewrite the whole
	# label. (RichTextLabel append/delete/scroll paths hit a text-shaping spin
	# on the A133 build; plain Label layout is far simpler.)
	_log_lines.append(text)
	while _log_lines.size() > MAX_LOG_LINES:
		_log_lines.pop_front()
	log.text = "\n".join(_log_lines)

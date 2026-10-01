extends Control
## Task 6: test_midi scene — one script, two modes (spec §8.4):
##
## GUI mode (default; also the on-device UI for Task 7): port-list Labels
## (inputs/outputs — plain Text Labels, NOT RichTextLabel), Open In /
## Open Out (PM device index from the SpinBox), Route In -> instance /
## Route Out <- instance buttons, "Send test note"
## (instance.send_midi(0, 60, 100)), a live event Label fed by all 8
## midi_* signals (20-line ring buffer, one label.text rewrite per event
## — the test_main.gd pattern; the A133 RichTextLabel lesson applies),
## and the MIDI-Learn demo: a checkbox plus two Labels; when armed, the
## first captured midi_cc shows controller/value.
##
## --midi-smoke (headless self-test): find an IAC input + matching IAC
## output (device names contain "IAC"). If none: print
## "MIDI_SMOKE_SKIP no IAC bus" and quit(0). Else: open both, route both
## to the instance, send_midi(0, 60, 100), then within 5 s wait for BOTH
## (a) the instance print of the note ("test_patch") AND (b) a
## midi_note_on for pitch 60 (IAC loopback: our output -> IAC bus -> our
## input). Both: MIDI_SMOKE_OK, quit(0). Timeout: dump state, quit(1).

const PATCH := "res://data/test_patch.pd"
# Smoke mode needs a patch that echoes [notein] back out via [noteout] so the
# IAC loopback is observable; test_patch.pd only prints (audio demo patch).
const SMOKE_PATCH := "res://data/smoke_patch.pd"
const MAX_LOG_LINES := 20
const SMOKE_TIMEOUT_MS := 5000
const SMOKE_WATCHDOG_MS := 15000
const SMOKE_TEARDOWN_GRACE_S := 0.1

@onready var inputs_label: Label = $VBox/InputsLabel
@onready var outputs_label: Label = $VBox/OutputsLabel
@onready var in_index_spin: SpinBox = $VBox/OpenRow/InIndexSpin
@onready var open_in_btn: Button = $VBox/OpenRow/OpenInBtn
@onready var open_out_btn: Button = $VBox/OpenRow/OpenOutBtn
@onready var route_in_btn: Button = $VBox/RouteRow/RouteInBtn
@onready var route_out_btn: Button = $VBox/RouteRow/RouteOutBtn
@onready var note_btn: Button = $VBox/SendNoteBtn
@onready var learn_check: CheckBox = $VBox/LearnCheck
@onready var learn_ctrl_label: Label = $VBox/LearnRow/LearnCtrlLabel
@onready var learn_val_label: Label = $VBox/LearnRow/LearnValLabel
@onready var event_log: Label = $VBox/EventLog

var _instance: LibpdInstance = null
var _instance_ready := false
var _in_port := -1
var _out_port := -1
var _log_lines: Array = []
var _learn_armed := false

# --midi-smoke state.
var _smoke_active := false
var _smoke_start := 0
var _smoke_got_print := false
var _smoke_got_note := false
var _smoke_done := false


func _ready() -> void:
	# The scene-owned instance (child node): init + load the patch +
	# start_dsp. Created at runtime (not as a .tscn node) to match the
	# test_main.gd / test_instance.gd convention. Smoke mode loads the
	# echo patch (noteout); GUI mode keeps the audio demo patch.
	# Arguments after the "--" separator land in get_cmdline_user_args()
	# (Godot 4.6.2); the brief's documented command uses "--", so both
	# sources are checked (the no-separator form is covered too).
	var cmdline := OS.get_cmdline_args() + OS.get_cmdline_user_args()
	var smoke := "--midi-smoke" in cmdline
	_instance = LibpdInstance.new()
	add_child(_instance)
	var mix_rate := int(AudioServer.get_mix_rate())
	var ok := _instance.init(mix_rate)
	if ok:
		ok = _instance.load_patch(SMOKE_PATCH if smoke else PATCH) == Error.OK
	if ok:
		ok = _instance.start_dsp() == Error.OK
	_instance_ready = ok
	if not _instance_ready:
		push_warning("test_midi: instance init/load/start failed")
	if smoke:
		_smoke_active = true
		_smoke_start = Time.get_ticks_msec()
		_run_smoke()
		return
	open_in_btn.pressed.connect(_on_open_in_pressed)
	open_out_btn.pressed.connect(_on_open_out_pressed)
	route_in_btn.pressed.connect(_on_route_in_pressed)
	route_out_btn.pressed.connect(_on_route_out_pressed)
	note_btn.pressed.connect(_on_note_pressed)
	learn_check.toggled.connect(_on_learn_toggled)
	Libpd.server.midi_note_on.connect(_on_midi_note_on)
	Libpd.server.midi_note_off.connect(_on_midi_note_off)
	Libpd.server.midi_cc.connect(_on_midi_cc)
	Libpd.server.midi_program_change.connect(_on_midi_program_change)
	Libpd.server.midi_pitch_bend.connect(_on_midi_pitch_bend)
	Libpd.server.midi_aftertouch.connect(_on_midi_aftertouch)
	Libpd.server.midi_poly_aftertouch.connect(_on_midi_poly_aftertouch)
	Libpd.server.midi_sysex.connect(_on_midi_sysex)
	Libpd.server.midi_port_error.connect(_on_midi_port_error)
	_refresh_port_lists()
	_event("test_midi ready (instance %s)" % ("ok" if _instance_ready else "FAILED"))
	_event("IAC demo: Open In + Open Out at the same bus index, Route both, Send test note")


func _process(_delta: float) -> void:
	# Wall-clock watchdog (test_main.gd pattern): force-exit if the smoke
	# has not finished in time. Only active in --midi-smoke mode.
	if _smoke_active and not _smoke_done:
		if Time.get_ticks_msec() - _smoke_start > SMOKE_WATCHDOG_MS:
			print("MIDI_SMOKE_FAIL watchdog (no finish in %d ms)" % SMOKE_WATCHDOG_MS)
			_dump_smoke_state()
			_smoke_done = true
			get_tree().quit(2)


# --------------------------------------------------------------------------
# --midi-smoke headless self-test (IAC loopback)
# --------------------------------------------------------------------------
func _run_smoke() -> void:
	print("MIDI_SMOKE | start instance_ready=%s" % str(_instance_ready))
	if not _instance_ready:
		await _smoke_fail("instance not ready")
		return
	if not Libpd.server.midi_available():
		print("MIDI_SMOKE_SKIP no IAC bus")
		await _finish_smoke(0)
		return
	var in_idx: int
	var out_idx: int
	# Fetch the device lists once: _find_iac_pair and the diagnostic print
	# share them instead of re-enumerating the PM devices.
	var in_list := Libpd.server.midi_list_inputs()
	var out_list := Libpd.server.midi_list_outputs()
	var pair := _find_iac_pair(in_list, out_list)
	in_idx = pair[0]
	out_idx = pair[1]
	print("MIDI_SMOKE | inputs=%s outputs=%s iac=(%d,%d)" % [
			str(in_list), str(out_list), in_idx, out_idx])
	if in_idx < 0 or out_idx < 0:
		print("MIDI_SMOKE_SKIP no IAC bus")
		await _finish_smoke(0)
		return
	_in_port = Libpd.server.midi_open_input(in_idx)
	_out_port = Libpd.server.midi_open_output(out_idx)
	if _in_port < 0 or _out_port < 0:
		await _smoke_fail("open failed (iac in_idx=%d out_idx=%d in_port=%d out_port=%d)" % [
				in_idx, out_idx, _in_port, _out_port])
		return
	Libpd.server.midi_route_input(_in_port, _instance)
	Libpd.server.midi_route_output(_instance, _out_port)
	Libpd.server.instance_print.connect(func(_id: int, text: String) -> void:
		if "test_patch" in text:
			_smoke_got_print = true)
	Libpd.server.midi_note_on.connect(func(_port: int, _ch: int, pitch: int, _vel: int) -> void:
		if pitch == 60:
			_smoke_got_note = true)
	_instance.send_midi(0, 60, 100)
	print("MIDI_SMOKE | note sent, waiting up to %d ms for print + looped note_on" % SMOKE_TIMEOUT_MS)
	while not (_smoke_got_print and _smoke_got_note):
		if Time.get_ticks_msec() - _smoke_start >= SMOKE_TIMEOUT_MS:
			break
		await get_tree().process_frame
	if _smoke_got_print and _smoke_got_note:
		print("MIDI_SMOKE_OK print=1 note=1 (iac in_idx=%d out_idx=%d)" % [in_idx, out_idx])
		await _finish_smoke(0)
		return
	await _smoke_fail("timeout print=%s note=%s (iac in_idx=%d out_idx=%d)" % [
			str(_smoke_got_print), str(_smoke_got_note), in_idx, out_idx])


func _smoke_fail(reason: String) -> void:
	print("MIDI_SMOKE_FAIL %s" % reason)
	_dump_smoke_state()
	await _finish_smoke(1)


func _finish_smoke(code: int) -> void:
	_smoke_done = true
	if _in_port >= 0:
		Libpd.server.midi_close_input(_in_port)
	if _out_port >= 0:
		Libpd.server.midi_close_output(_out_port)
	if _instance != null and _instance_ready:
		_instance.stop_dsp()
		_instance.queue_free()
		# Grace period before quit: the AudioServer holds a Ref to the
		# instance's AudioStreamGeneratorPlayback in its playback_list until
		# its (dummy in --headless) audio thread processes the FADE_OUT and
		# unrefs it. One frame is sub-millisecond in headless and can quit
		# before that, leaving "ObjectDB instances leaked at exit".
		# test_main.gd's timed awaits give the same grace implicitly.
		await get_tree().create_timer(SMOKE_TEARDOWN_GRACE_S).timeout
	get_tree().quit(code)


func _dump_smoke_state() -> void:
	if _instance != null:
		print("state | instance_id=%d ready=%s blocks=%d" % [
				_instance.instance_id, str(_instance_ready),
				int(_instance.debug_blocks_pushed)])
	print("state | in_port=%d out_port=%d got_print=%s got_note=%s" % [
			_in_port, _out_port, str(_smoke_got_print), str(_smoke_got_note)])


func _find_iac_pair(in_ports: Array, out_ports: Array) -> Array:
	# Matching IAC in/out pair: an IAC bus names both of its ends the same
	# ("IAC Bus N"), so loopback needs the pair with equal names. If the
	# names never line up, fall back to the first IAC in + first IAC out.
	for d_in in in_ports:
		if "IAC" not in str(d_in["name"]):
			continue
		for d_out in out_ports:
			if str(d_out["name"]) == str(d_in["name"]):
				return [int(d_in["index"]), int(d_out["index"])]
	return [_find_iac_index(in_ports), _find_iac_index(out_ports)]


func _find_iac_index(ports: Array) -> int:
	# First device whose name contains "IAC" (an IAC Driver bus); -1 if none.
	for d in ports:
		if "IAC" in str(d["name"]):
			return int(d["index"])
	return -1


# --------------------------------------------------------------------------
# UI handlers (GUI mode only)
# --------------------------------------------------------------------------
func _on_open_in_pressed() -> void:
	# Re-opening closes the previous port first (port ids are never
	# reused; a second open would leak the first one).
	if _in_port >= 0:
		Libpd.server.midi_close_input(_in_port)
		_in_port = -1
	var idx := int(in_index_spin.value)
	_in_port = Libpd.server.midi_open_input(idx)
	if _in_port < 0:
		_event("failed to open input (pm index %d)" % idx)
	else:
		_event("opened input port=%d (pm index %d)" % [_in_port, idx])


func _on_open_out_pressed() -> void:
	if _out_port >= 0:
		Libpd.server.midi_close_output(_out_port)
		_out_port = -1
	var idx := int(in_index_spin.value)
	_out_port = Libpd.server.midi_open_output(idx)
	if _out_port < 0:
		_event("failed to open output (pm index %d)" % idx)
	else:
		_event("opened output port=%d (pm index %d)" % [_out_port, idx])


func _on_route_in_pressed() -> void:
	if _in_port < 0:
		_event("route in: open an input first")
		return
	Libpd.server.midi_route_input(_in_port, _instance)
	_event("routed input port=%d -> instance" % _in_port)


func _on_route_out_pressed() -> void:
	if _out_port < 0:
		_event("route out: open an output first")
		return
	Libpd.server.midi_route_output(_instance, _out_port)
	_event("routed instance -> output port=%d" % _out_port)


func _on_note_pressed() -> void:
	if not _instance_ready:
		_event("send note: instance not ready")
		return
	_instance.send_midi(0, 60, 100)
	_event("sent note ch=0 pitch=60 vel=100")


func _on_learn_toggled(armed: bool) -> void:
	_learn_armed = armed
	if armed:
		learn_ctrl_label.text = "controller: waiting"
		learn_val_label.text = "value: waiting"
		_event("MIDI-Learn armed: waiting for first CC")
	else:
		_event("MIDI-Learn disarmed")


# --------------------------------------------------------------------------
# LibpdServer midi_* signals (GUI mode; live event Label)
# --------------------------------------------------------------------------
func _on_midi_note_on(port: int, ch: int, pitch: int, vel: int) -> void:
	_event("note_on port=%d ch=%d pitch=%d vel=%d" % [port, ch, pitch, vel])


func _on_midi_note_off(port: int, ch: int, pitch: int, vel: int) -> void:
	_event("note_off port=%d ch=%d pitch=%d vel=%d" % [port, ch, pitch, vel])


func _on_midi_cc(port: int, ch: int, controller: int, value: int) -> void:
	_event("cc port=%d ch=%d ctrl=%d val=%d" % [port, ch, controller, value])
	if _learn_armed:
		# First captured CC while armed: show it, then disarm (one-shot
		# capture; the check no longer re-arms until the user ticks it).
		_learn_armed = false
		learn_ctrl_label.text = "controller: %d" % controller
		learn_val_label.text = "value: %d" % value
		learn_check.set_pressed_no_signal(false)
		_event("MIDI-Learn captured ctrl=%d val=%d" % [controller, value])


func _on_midi_program_change(port: int, ch: int, program: int) -> void:
	_event("prog port=%d ch=%d program=%d" % [port, ch, program])


func _on_midi_pitch_bend(port: int, ch: int, value: int) -> void:
	_event("bend port=%d ch=%d val=%d" % [port, ch, value])


func _on_midi_aftertouch(port: int, ch: int, value: int) -> void:
	_event("aftertouch port=%d ch=%d val=%d" % [port, ch, value])


func _on_midi_poly_aftertouch(port: int, ch: int, pitch: int, value: int) -> void:
	_event("poly port=%d ch=%d pitch=%d val=%d" % [port, ch, pitch, value])


func _on_midi_sysex(port: int, data: PackedByteArray) -> void:
	_event("sysex port=%d len=%d" % [port, data.size()])


func _on_midi_port_error(port: int, what: String) -> void:
	_event("port_error port=%d: %s" % [port, what])


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
func _refresh_port_lists() -> void:
	if not Libpd.server.midi_available():
		inputs_label.text = "MIDI inputs: (MIDI not available on this platform)"
		outputs_label.text = "MIDI outputs: (MIDI not available on this platform)"
		return
	inputs_label.text = "MIDI inputs:\n" + _format_ports(Libpd.server.midi_list_inputs())
	outputs_label.text = "MIDI outputs:\n" + _format_ports(Libpd.server.midi_list_outputs())


func _format_ports(ports: Array) -> String:
	if ports.is_empty():
		return "(none)"
	var lines: Array = []
	for d in ports:
		lines.append("%d: %s" % [d["index"], d["name"]])
	return "\n".join(lines)


func _event(text: String) -> void:
	# Mirror every event to stdout with the [MIDI] prefix: on the A133 the
	# on-screen Label is the only visible log, but adb-captured stdout is
	# the debugging channel (Task 7 on-device loopback verification).
	print("[MIDI] " + text)
	# Plain Label log: keep a GDScript-side ring buffer and rewrite the whole
	# label per event (test_main.gd pattern; RichTextLabel is banned on the
	# A133 build).
	_log_lines.append(text)
	while _log_lines.size() > MAX_LOG_LINES:
		_log_lines.pop_front()
	event_log.text = "\n".join(_log_lines)

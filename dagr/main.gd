extends Control
## DAGR display fed by VBSAndroidOptics.dll (../plugin/dagr.cpp). Asks for a reading four times a second: to this
## PC, and broadcast on the network until a VBS answers. Keeps to the VBS that answered, preferring this PC's own.

const PORT := 47840
const REQUEST := "VDG1"
const POLL_SECONDS := 0.25
const LOST_AFTER_MS := 3000     # No reply this long: search again and show dashes
const LOCAL := "127.0.0.1"

var udp := PacketPeerUDP.new()
var vbs_address := ""           # The VBS PC that answered ("" = searching)
var last_reply_ms := -LOST_AFTER_MS
var poll_timer := 0.0
# JFSim's DAGR target mode: after a lase with both rangefinder buttons on the phone, until the observer moves
var last_target_id := -1        # Of the plugin's last lase (-1: not seen a reading yet)
var target := {}                # The lase being shown ({} = normal display)
var target_observer := ""       # The observer's grid when it was lased

@onready var mgrs_label: Label = $MGRS
@onready var date_label: Label = $Date
@onready var time_label: Label = $Time
@onready var elevation_label: Label = $Elevation
@onready var accuracy_label: Label = $Accuracy
@onready var battery_label: Label = $"Battery Locations"
@onready var header_label: Label = $"MGRS-New WGD"
@onready var date_caption: Label = $DateLabel
@onready var elevation_caption: Label = $ElevationLabel
@onready var accuracy_caption: Label = $AccuracyLabel


func _ready() -> void:
	udp.set_broadcast_enabled(true)
	udp.bind(0)
	show_no_reading()
	come_to_front()


## Starts in front of VBS (the plugin allows it to take the foreground when it starts the app), but is not kept on top
func come_to_front() -> void:
	DisplayServer.window_move_to_foreground()
	get_window().grab_focus()


func _process(delta: float) -> void:
	if target.is_empty():
		date_label.text = today()
	poll_timer -= delta
	if poll_timer <= 0.0:
		poll_timer = POLL_SECONDS
		poll()
	while udp.get_available_packet_count() > 0:
		var data := udp.get_packet().get_string_from_utf8()
		var from := udp.get_packet_ip()
		var reading = JSON.parse_string(data)
		if typeof(reading) != TYPE_DICTIONARY:
			continue
		# This PC's VBS wins over any other that answered the broadcast
		if vbs_address == "" or from == LOCAL or (from != vbs_address and not connected()):
			vbs_address = from
		if from != vbs_address:
			continue
		last_reply_ms = Time.get_ticks_msec()
		if reading.get("ok", false):
			show_reading(reading)
		else:
			show_no_reading()
	if vbs_address != "" and not connected():
		vbs_address = ""
		show_no_reading()


## Escape switches between fullscreen and a window
func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		var window := get_window()
		if window.mode == Window.MODE_FULLSCREEN or window.mode == Window.MODE_EXCLUSIVE_FULLSCREEN:
			window.mode = Window.MODE_WINDOWED
		else:
			window.mode = Window.MODE_FULLSCREEN


func connected() -> bool:
	return Time.get_ticks_msec() - last_reply_ms < LOST_AFTER_MS


func poll() -> void:
	var targets: Array[String] = [LOCAL]
	if vbs_address != "" and vbs_address != LOCAL:
		targets = [vbs_address]
	elif vbs_address == "":
		targets.append("255.255.255.255")
	for address in targets:
		udp.set_dest_address(address, PORT)
		udp.put_packet(REQUEST.to_ascii_buffer())


func show_reading(reading: Dictionary) -> void:
	var observer := str(reading.get("mgrs", ""))
	follow_target(reading, observer)
	time_label.text = str(reading.get("time", "--:--:--"))
	if target.is_empty():
		show_normal()
		mgrs_label.text = grid_text(observer)
		elevation_label.text = "%d m" % roundi(float(reading.get("elevation", 0.0)))
	else:
		# As JFSim's DAGR shows a lase: the target's grid, direction, distance and difference in altitude
		header_label.text = " LRF TGT MODE - Target Grid:"
		header_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_LEFT
		mgrs_label.text = grid_text(str(target.get("mgrs", "")))
		elevation_caption.text = " Dn"
		elevation_label.text = "%d mils" % int(target.get("dn_mils", 0))
		date_caption.text = " Dist"
		date_label.text = "%d m" % int(target.get("dist", 0))
		accuracy_caption.text = " DifAlt"
		accuracy_label.text = "%d m" % int(target.get("dif_alt", 0))
	var batteries: Array = []
	for b in reading.get("batteries", []):
		var g := split_mgrs(str(b.get("mgrs", "")))
		if not g.is_empty():
			batteries.append("C/S %d: %s %s %s %s " % [int(b.get("cs", 0)), g[0], g[1], g[2].left(3), g[3].left(3)])
	battery_label.text = battery_text(batteries, "%d" % int(reading.get("mv_mils", 0)))


## A new lase starts target mode; the observer moving, or the lase going (mission ended), ends it
func follow_target(reading: Dictionary, observer: String) -> void:
	var t: Variant = reading.get("target")
	var id := int(t.get("id", 0)) if t is Dictionary else 0
	if last_target_id < 0:
		last_target_id = id  # A lase from before the app started is not shown
	elif id != last_target_id:
		last_target_id = id
		if id > 0:
			target = t
			target_observer = observer
	if not target.is_empty() and (id == 0 or observer != target_observer):
		target = {}


func show_no_reading() -> void:
	target = {}
	last_target_id = -1
	show_normal()
	mgrs_label.text = grid_text("")
	time_label.text = "--:--:--"
	elevation_label.text = "--- m"
	battery_label.text = battery_text([], "---")


## The normal captions, and the fixed accuracy
func show_normal() -> void:
	header_label.text = " MGRS-New\tWGD"
	header_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_FILL  # Spreads MGRS-New and WGD to the edges
	date_caption.text = " Date"
	elevation_caption.text = " Elevation"
	accuracy_caption.text = " Accuracy"
	accuracy_label.text = "+/- 7m"


## The draft's layout: heading, then up to 5 batteries a line apart, Compass MV always on the same line.
## No batteries: no heading either.
func battery_text(batteries: Array, mv: String) -> String:
	var lines: Array[String] = []
	lines.resize(14)
	lines.fill("")
	if not batteries.is_empty():
		lines[0] = "Battery Locations: "
		for i in mini(batteries.size(), 5):
			lines[2 + i * 2] = batteries[i]
	lines[13] = "Compass MV: %s mils " % mv
	return "\n".join(lines)


## "60HUB9461034250" -> the draft's two lines, "60H UB 94610 e" over "34250 n"; dashes if it is not MGRS
func grid_text(mgrs: String) -> String:
	var grid := split_mgrs(mgrs)
	if grid.is_empty():
		return "-- -- ----- e \n----- n "
	return "%s %s %s e \n%s n " % [grid[0], grid[1], grid[2], grid[3]]


## "60HUB9461034250" -> ["60H", "UB", "94610", "34250"]; [] if it is not MGRS
func split_mgrs(mgrs: String) -> Array[String]:
	var re := RegEx.create_from_string("^(\\d{1,2}[A-Z])([A-Z]{2})(\\d+)$")
	var m := re.search(mgrs.replace(" ", ""))
	if m == null or m.get_string(3).length() % 2 != 0:
		return []
	var digits := m.get_string(3)
	var half := digits.length() / 2
	return [m.get_string(1), m.get_string(2), digits.left(half), digits.right(half)]


## Today's date from this PC's clock, as in the draft: DD-MM-YY
func today() -> String:
	var d := Time.get_date_dict_from_system()
	return "%02d-%02d-%02d" % [d.day, d.month, d.year % 100]

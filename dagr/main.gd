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
# The ☰ menu (Escape): the binos' view mode and the phone's lased grid, sent to the plugin with each request
# ("VDG1m<mode>g<0|1>") and remembered here
const VIEW_MODES := ["Day Optics", "Night Vision", "Thermal Black Hot", "Thermal White Hot", "Thermal Fusion"]
const GRID_ITEM := 10
const SETTINGS := "user://dagr.cfg"
var view_mode := 0
var show_grid := true
var menu_button: Button
var menu: PopupMenu

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
	load_settings()
	build_menu()
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


## Escape leaves fullscreen and shows the ☰ menu button; Escape again hides it and goes back to fullscreen
func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		set_menu_mode(not menu_button.visible)


func set_menu_mode(on: bool) -> void:
	menu_button.visible = on
	if not on:
		menu.hide()
	get_window().mode = Window.MODE_WINDOWED if on else Window.MODE_FULLSCREEN


## The ☰ button (top right, three bars) and its menu, in the DAGR's own font
func build_menu() -> void:
	var font: Font = load("res://Jersey_10/Jersey10-Regular.ttf")
	menu_button = Button.new()
	menu_button.anchor_left = 1.0
	menu_button.anchor_right = 1.0
	menu_button.offset_left = -100
	menu_button.offset_right = -24
	menu_button.offset_top = 24
	menu_button.offset_bottom = 88
	menu_button.visible = false
	menu_button.focus_mode = Control.FOCUS_NONE
	for i in 3:
		var bar := ColorRect.new()
		bar.color = Color(0.82, 0.8, 0.62)
		bar.position = Vector2(16, 16 + i * 14)
		bar.size = Vector2(44, 6)
		bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
		menu_button.add_child(bar)
	add_child(menu_button)

	menu = PopupMenu.new()
	menu.add_theme_font_override("font", font)
	menu.add_theme_font_size_override("font_size", 40)
	for i in VIEW_MODES.size():
		menu.add_radio_check_item(VIEW_MODES[i], i)
	menu.add_separator()
	menu.add_check_item("Show LRF Grid Reference", GRID_ITEM)
	menu.hide_on_checkable_item_selection = false
	add_child(menu)
	update_menu_checks()

	menu_button.pressed.connect(func():
		menu.reset_size()
		var right := menu_button.global_position.x + menu_button.size.x
		menu.position = Vector2i(int(right) - menu.size.x, int(menu_button.global_position.y + menu_button.size.y + 8))
		menu.popup())
	menu.id_pressed.connect(func(id: int):
		if id == GRID_ITEM:
			show_grid = not show_grid
		elif id >= 0 and id < VIEW_MODES.size():
			view_mode = id
		update_menu_checks()
		save_settings()
		poll()) # tell the plugin straight away


func update_menu_checks() -> void:
	for i in VIEW_MODES.size():
		menu.set_item_checked(menu.get_item_index(i), i == view_mode)
	menu.set_item_checked(menu.get_item_index(GRID_ITEM), show_grid)


func load_settings() -> void:
	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS) == OK:
		view_mode = clampi(int(cfg.get_value("binos", "view_mode", 0)), 0, VIEW_MODES.size() - 1)
		show_grid = bool(cfg.get_value("binos", "show_grid", true))


func save_settings() -> void:
	var cfg := ConfigFile.new()
	cfg.set_value("binos", "view_mode", view_mode)
	cfg.set_value("binos", "show_grid", show_grid)
	cfg.save(SETTINGS)


func connected() -> bool:
	return Time.get_ticks_msec() - last_reply_ms < LOST_AFTER_MS


func poll() -> void:
	var targets: Array[String] = [LOCAL]
	if vbs_address != "" and vbs_address != LOCAL:
		targets = [vbs_address]
	elif vbs_address == "":
		targets.append("255.255.255.255")
	for address in targets:
		# The menu's choices go only to the VBS this DAGR is locked onto, never in a broadcast
		var request := REQUEST
		if address == vbs_address:
			request += "m%dg%d" % [view_mode, 1 if show_grid else 0]
		udp.set_dest_address(address, PORT)
		udp.put_packet(request.to_ascii_buffer())


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

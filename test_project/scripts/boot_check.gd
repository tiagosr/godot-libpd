extends Node2D
# Task 1 boot check: the GDExtension is loaded and LibpdServer is registered
# as a class and instantiated as the project singleton.

func _ready() -> void:
	if not ClassDB.class_exists("LibpdServer"):
		push_error("LibpdServer class missing")
		get_tree().quit(1)
		return
	if Libpd.server == null:
		push_error("LibpdServer singleton missing")
		get_tree().quit(1)
		return
	print("LIBPD_BOOT_OK")
	get_tree().quit(0)

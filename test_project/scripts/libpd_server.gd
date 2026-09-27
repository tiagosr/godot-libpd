extends Node
## Autoload wrapper for the C++ LibpdServer extension class.
##
## The C++ class must NOT be the autoload type itself: autoload scripts are
## parsed before extension classes are registered, so `extends LibpdServer`
## would fail. This plain Node creates the singleton in _ready (after
## extension registration) and exposes it as `Libpd.server`.

@onready var server: LibpdServer = LibpdServer.new()


func _ready() -> void:
	add_child(server)

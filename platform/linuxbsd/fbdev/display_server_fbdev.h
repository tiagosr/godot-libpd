/**************************************************************************/
/*  display_server_fbdev.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                        */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Casmuzo.                */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and/or    */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACTING   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef FBDEV_ENABLED

#include <atomic>

#include "core/os/mutex.h"
#include "platform_gl.h"
#include "servers/display/display_server.h"

/**
 * Display server for Knulli-class Allwinner handhelds (Trimui Brick A133
 * and similar) — no X11, no Wayland, no DRM-KMS.
 *
 * Display architecture (reverse-engineered on-device):
 *   - The panel is scanned out by the vendor PowerVR display controller
 *     (pvrsrvkm / `disp`). A process claims the display by creating an
 *     EGL *window* surface — the vendor NULL-window-system backend
 *     (libpvrNULL_WSEGL, WSEGL_CreateWindowDrawable, logs
 *     "MALI_CreateWindow") allocates the DC buffer — and then
 *     eglSwapBuffers() presents to the LCD.
 *   - /dev/fb0 is a separate simple framebuffer that EmulationStation
 *     also writes; it is NOT the port display path. Writing there while
 *     ES runs only produces flicker, and it is not scanned out once a
 *     port takes the DC.
 *
 * Rendering model:
 *   - One fixed fullscreen "window" (1024x768 on the Brick, from fb0
 *     geometry when available).
 *   - The GLES3 compatibility rasterizer renders into the EGL
 *     WindowSurface's default framebuffer; swap_buffers() presents via
 *     eglSwapBuffers() and paces to ~60fps (GODOT_FBDEV_FPS).
 *   - Legacy path (GODOT_FBDEV_FB0=1): glReadPixels back to mmap'd
 *     /dev/fb0, for diagnostics only.
 *
 * The EGL and GLES entry points come from the engine's glad loader,
 * which dlopens libEGL.so.1 / libGLESv2.so.2 at runtime — no link-time
 * GL dependencies are added.
 */
class DisplayServerFbdev : public DisplayServer {
	GDSOFTCLASS(DisplayServerFbdev, DisplayServer);

private:
	friend class DisplayServer;

	// EGL state (single display, one window surface, one context).
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLSurface egl_surface = EGL_NO_SURFACE;
	bool egl_ok = false;

	// Framebuffer state. fb0 is queried read-only for the panel geometry;
	// the legacy glReadPixels->fb0 blit (GODOT_FBDEV_FB0=1) mmaps it.
	int fb_fd = -1;
	uint8_t *fb_mem = nullptr;
	size_t fb_map_size = 0;
	uint32_t fb_width = 0;
	uint32_t fb_height = 0;
	uint32_t fb_stride = 0; // bytes per row

	Vector<uint8_t> readback;
	uint64_t last_swap_usec = 0;

	// Evdev input (firmware uinput gamepad + key sources, e.g. the Trimui
	// Brick's `trimui_inputd` -> /dev/uinput -> /dev/input/eventN).
	struct EvdevDev {
		int fd = -1;
		char name[64] = { 0 };
		// axis/edge state for press/release synthesis
		int hat_x = 0, hat_y = 0, abs_x = 0, abs_y = 0;
		bool start_held = false, select_held = false;
	};
	Vector<EvdevDev> evdev_devs;
	Thread evdev_thread;
	std::atomic<bool> evdev_stop{ false };

	// Cross-thread input handoff: the evdev thread only ever appends to
	// this queue; process_events() (main thread, once per iteration)
	// delivers the events to Input. Calling Input/SceneTree APIs from the
	// evdev thread directly is not safe (node callbacks + GDScript run on
	// the main thread only).
	Mutex evdev_queue_mutex;
	Vector<Ref<InputEvent>> evdev_queue;
	bool evdev_quit_requested = false;
	std::atomic<uint64_t> evdev_push_count{ 0 }; // actual events pushed to the queue

	// Logical press dedup (evdev thread only): firmware input layers can
	// expose the same physical button through several event devices (a
	// controller mirror + a keyboard mirror). Track the logical pressed
	// state per key/joypad-button so a press is emitted once and a release
	// once, no matter how many devices report it (SDL model: per-device
	// channel classification + state-based polling).
	struct EvdevDedup {
		int64_t id = 0;
		bool pressed = false;
	};
	Vector<EvdevDedup> evdev_dedup;

	// Global D-pad state (evdev thread only): all mirror devices report
	// the same hat direction, so one logical edge = one press, even when
	// two devices deliver the transition.
	int evdev_hat_x = 0;
	int evdev_hat_y = 0;

	void evdev_thread_main();
	static void _evdev_thread_trampoline(void *p_user);
	void evdev_push_key(Key p_key, int p_joypad_btn, bool p_pressed, bool p_echo);
	bool evdev_dedup_state(int64_t p_id, bool p_is_joy, bool p_pressed, bool p_echo, bool &r_allow);

	NativeMenu *native_menu = nullptr;
	Callable input_event_callback;

	Error _init_fb();
	bool _init_egl();

	static Vector<String> get_rendering_drivers_func() {
		Vector<String> drivers;
		drivers.push_back("opengl3_es");
		return drivers;
	}

	static DisplayServer *create_func(const String &p_rendering_driver, DisplayServer::WindowMode p_mode, DisplayServer::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, Context p_context, int64_t p_parent_window, Error &r_error);

	static void _dispatch_input_events(const Ref<InputEvent> &p_event) {
		static_cast<DisplayServerFbdev *>(get_singleton())->_dispatch_input_event(p_event);
	}

	void _dispatch_input_event(const Ref<InputEvent> &p_event) {
		if (input_event_callback.is_valid()) {
			input_event_callback.call(p_event);
		}
	}

public:
	static void register_fbdev_driver();

	// Must succeed before the rasterizer is created.
	Error init_fbdev();

	DisplayServerFbdev();
	~DisplayServerFbdev();

	bool has_feature(Feature p_feature) const override { return false; }
	String get_name() const override { return "fbdev"; }

	// --- Screen / window: one fixed fullscreen window = the framebuffer. ---

	int get_screen_count() const override { return 1; }
	int get_primary_screen() const override { return 0; }
	Point2i screen_get_position(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return Point2i(); }
	Size2i screen_get_size(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return Size2i(fb_width, fb_height); }
	Rect2i screen_get_usable_rect(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return Rect2i(Point2i(), Size2i(fb_width, fb_height)); }
	int screen_get_dpi(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return 96; /* 0 might cause issues */ }
	float screen_get_scale(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return 1; }
	float screen_get_max_scale() const override { return 1; }
	float screen_get_refresh_rate(int p_screen = SCREEN_OF_MAIN_WINDOW) const override { return 60; }
	void screen_set_orientation(ScreenOrientation p_orientation, int p_screen = SCREEN_OF_MAIN_WINDOW) override {}
	void screen_set_keep_on(bool p_enable) override {}

	Vector<DisplayServer::WindowID> get_window_list() const override { return Vector<DisplayServer::WindowID>(); }

	WindowID create_sub_window(WindowMode p_mode, VSyncMode p_vsync_mode, uint32_t p_flags, const Rect2i &p_rect = Rect2i(), bool p_exclusive = false, WindowID p_transient_parent = INVALID_WINDOW_ID) override { return 0; }
	void show_window(WindowID p_id) override {}
	void delete_sub_window(WindowID p_id) override {}

	WindowID get_window_at_screen_position(const Point2i &p_position) const override { return 0; }

	void window_attach_instance_id(ObjectID p_instance, WindowID p_window = MAIN_WINDOW_ID) override {}
	ObjectID window_get_attached_instance_id(WindowID p_window = MAIN_WINDOW_ID) const override { return ObjectID(); }

	void window_set_rect_changed_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override {}

	void window_set_window_event_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override {}

	void window_set_input_event_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override {
		input_event_callback = p_callable;
	}

	void window_set_input_text_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override {}
	void window_set_drop_files_callback(const Callable &p_callable, WindowID p_window = MAIN_WINDOW_ID) override {}

	void window_set_title(const String &p_title, WindowID p_window = MAIN_WINDOW_ID) override {}

	void window_set_mouse_passthrough(const Vector<Vector2> &p_region, WindowID p_window = MAIN_WINDOW_ID) override {}

	int window_get_current_screen(WindowID p_window = MAIN_WINDOW_ID) const override { return 0; }
	void window_set_current_screen(int p_screen, WindowID p_window = MAIN_WINDOW_ID) override {}

	Point2i window_get_position(WindowID p_window = MAIN_WINDOW_ID) const override { return Point2i(); }
	Point2i window_get_position_with_decorations(WindowID p_window = MAIN_WINDOW_ID) const override { return Point2i(); }
	void window_set_position(const Point2i &p_position, WindowID p_window = MAIN_WINDOW_ID) override {}

	void window_set_transient(WindowID p_window, WindowID p_parent) override {}

	void window_set_max_size(const Size2i p_size, WindowID p_window = MAIN_WINDOW_ID) override {}
	Size2i window_get_max_size(WindowID p_window = MAIN_WINDOW_ID) const override { return Size2i(fb_width, fb_height); }

	void window_set_min_size(const Size2i p_size, WindowID p_window = MAIN_WINDOW_ID) override {}
	Size2i window_get_min_size(WindowID p_window = MAIN_WINDOW_ID) const override { return Size2i(fb_width, fb_height); }

	void window_set_size(const Size2i p_size, WindowID p_window = MAIN_WINDOW_ID) override {}
	Size2i window_get_size(WindowID p_window = MAIN_WINDOW_ID) const override { return Size2i(fb_width, fb_height); }
	Size2i window_get_size_with_decorations(WindowID p_window = MAIN_WINDOW_ID) const override { return Size2i(fb_width, fb_height); }

	void window_set_mode(WindowMode p_mode, WindowID p_window = MAIN_WINDOW_ID) override {}
	WindowMode window_get_mode(WindowID p_window = MAIN_WINDOW_ID) const override { return WINDOW_MODE_FULLSCREEN; }

	void window_set_vsync_mode(VSyncMode p_vsync_mode, WindowID p_window = MAIN_WINDOW_ID) override {}
	VSyncMode window_get_vsync_mode(WindowID p_window) const override { return VSyncMode::VSYNC_DISABLED; }

	bool window_is_maximize_allowed(WindowID p_window = MAIN_WINDOW_ID) const override { return false; }

	void window_set_flag(WindowFlags p_flag, bool p_enabled, WindowID p_window = MAIN_WINDOW_ID) override {}
	bool window_get_flag(WindowFlags p_flag, WindowID p_window = MAIN_WINDOW_ID) const override { return false; }

	void window_request_attention(WindowID p_window = MAIN_WINDOW_ID) override {}
	void window_move_to_foreground(WindowID p_window = MAIN_WINDOW_ID) override {}
	bool window_is_focused(WindowID p_window = MAIN_WINDOW_ID) const override { return true; }

	bool window_can_draw(WindowID p_window = MAIN_WINDOW_ID) const override { return egl_ok; }

	bool can_any_window_draw() const override { return egl_ok; }

	void window_set_ime_active(const bool p_active, WindowID p_window = MAIN_WINDOW_ID) override {}
	void window_set_ime_position(const Point2i &p_pos, WindowID p_window = MAIN_WINDOW_ID) override {}

	int64_t window_get_native_handle(HandleType p_handle_type, WindowID p_window = MAIN_WINDOW_ID) const override { return 0; }

	void process_events() override;

	void set_native_icon(const String &p_filename) override {}
	void set_icon(const Ref<Image> &p_icon) override {}

	void help_set_search_callbacks(const Callable &p_search_callback = Callable(), const Callable &p_action_callback = Callable()) override {}

	bool tts_is_speaking() const override { return false; }
	bool tts_is_paused() const override { return false; }
	TypedArray<Dictionary> tts_get_voices() const override { return TypedArray<Dictionary>(); }
	void tts_speak(const String &p_text, const String &p_voice, int p_volume = 50, float p_pitch = 1.0f, float p_rate = 1.0f, int64_t p_utterance_id = 0, bool p_interrupt = false) override {}
	void tts_pause() override {}
	void tts_resume() override {}
	void tts_stop() override {}

	void mouse_set_mode(MouseMode p_mode) override {}
	void mouse_set_mode_override(MouseMode p_mode) override {}
	void mouse_set_mode_override_enabled(bool p_override_enabled) override {}
	Point2i mouse_get_position() const override { return Point2i(); }
	void clipboard_set(const String &p_text) override {}
	void clipboard_set_primary(const String &p_text) override {}

	void virtual_keyboard_show(const String &p_existing_text, const Rect2 &p_screen_rect = Rect2(), VirtualKeyboardType p_type = KEYBOARD_TYPE_DEFAULT, int p_max_length = -1, int p_cursor_start = -1, int p_cursor_end = -1) override {}
	void virtual_keyboard_hide() override {}

	void cursor_set_shape(CursorShape p_shape) override {}
	void cursor_set_custom_image(const Ref<Resource> &p_cursor, CursorShape p_shape = CURSOR_ARROW, const Vector2 &p_hotspot = Vector2()) override {}

	Error dialog_show(String p_title, String p_description, Vector<String> p_buttons, const Callable &p_callback) override { return ERR_UNAVAILABLE; }
	Error dialog_input_text(String p_title, String p_description, String p_partial, const Callable &p_callback) override { return ERR_UNAVAILABLE; }
	Error file_dialog_show(const String &p_title, const String &p_current_directory, const String &p_filename, bool p_show_hidden, FileDialogMode p_mode, const Vector<String> &p_filters, const Callable &p_callback, WindowID p_window_id) override { return ERR_UNAVAILABLE; }
	Error file_dialog_with_options_show(const String &p_title, const String &p_current_directory, const String &p_root, const String &p_filename, bool p_show_hidden, FileDialogMode p_mode, const Vector<String> &p_filters, const TypedArray<Dictionary> &p_options, const Callable &p_callback, WindowID p_window_id) override { return ERR_UNAVAILABLE; }

	void release_rendering_thread() override {}
	void swap_buffers() override;

	// GL context plumbing (called from the rendering thread).
	void gl_window_make_current(DisplayServer::WindowID p_window_id) override;

	IndicatorID create_status_indicator(const Ref<Texture2D> &p_icon, const String &p_tooltip, const Callable &p_callback) override { return INVALID_INDICATOR_ID; }
	void status_indicator_set_icon(IndicatorID p_id, const Ref<Texture2D> &p_icon) override {}
	void status_indicator_set_tooltip(IndicatorID p_id, const String &p_tooltip) override {}
	void status_indicator_set_callback(IndicatorID p_id, const Callable &p_callback) override {}
	void delete_status_indicator(IndicatorID p_id) override {}
};

#endif // FBDEV_ENABLED

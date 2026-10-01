/**************************************************************************/
/*  display_server_fbdev.cpp                                              */
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

#ifdef FBDEV_ENABLED

#include "display_server_fbdev.h"

#include "core/input/input.h"
#include "core/input/input_enums.h"
#include "core/os/os.h"
#include "drivers/gles3/rasterizer_gles3.h"
#include "platform_gl.h"
#include "scene/main/scene_tree.h"

// linux/input.h defines KEY_* macros that collide with Godot's Key enum
// (e.g. KEY_DELETE), so it must come AFTER all Godot headers.
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>

#ifdef __has_include
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#endif
#endif
#include <signal.h>

namespace {

// Debug aid: SIGUSR1 dumps the receiving thread's native backtrace to
// /tmp/godot_bt.log and keeps running. NOTE: backtrace()/
// backtrace_symbols_fd() are NOT POSIX async-signal-safe (glibc may
// allocate / call dlsym inside the handler); acceptable for a
// deliberately-installed diagnostic aid, not for arbitrary signals.
// Install with GODOT_FBDEV_BT=1.
void fbdev_bt_handler(int) {
	void *bt[128];
	int n = backtrace(bt, 128);
	int fd = ::open("/tmp/godot_bt.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		backtrace_symbols_fd(bt, n, fd);
		::close(fd);
	}
}
// Unbuffered crash-proof diagnostics: raw write() to a tmpfs file so the
// timeline survives segfaults and stdio buffering. Disable with
// GODOT_FBDEV_DIAG=0.
void fbdev_diag(const char *msg) {
	static int diag_fd = -2;
	if (diag_fd == -2) {
		const char *off = getenv("GODOT_FBDEV_DIAG");
		diag_fd = (off && off[0] == '0') ? -1 : open("/tmp/godot_fbdev_diag.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (diag_fd >= 0) {
		ssize_t w = write(diag_fd, msg, strlen(msg));
		(void)w;
	}
}

void fbdev_diagf(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fbdev_diag(buf);
}

// Bisection gates (diagnostics): disable one event class to isolate
// the D-pad main-thread spin.
static bool fbdev_evkey_on() {
	const char *e = getenv("GODOT_FBDEV_EVKEY");
	return !e || e[0] != '0';
}
static bool fbdev_evjoy_on() {
	const char *e = getenv("GODOT_FBDEV_EVJOY");
	return !e || e[0] != '0';
}

void fbdev_diag_egl_err(const char *what) {
	char buf[160];
	snprintf(buf, sizeof(buf), "[%s] FAILED (eglError=0x%04x)\n", what, eglGetError());
	fbdev_diag(buf);
}

// Raw evdev event logger (GODOT_FBDEV_EVLOG=1), same crash-proof pattern.
void fbdev_evdev_log(const char *name, const struct input_event &ev) {
	static int lfd = -2;
	if (lfd == -2) {
		const char *on = getenv("GODOT_FBDEV_EVLOG");
		lfd = (on && on[0] == '1') ? open("/tmp/godot_evdev.log", O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
	}
	if (lfd >= 0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		char buf[96];
		int n = snprintf(buf, sizeof(buf), "t=%ld.%03ld %s type=%d code=%d val=%d\n",
				(long)ts.tv_sec, (long)(ts.tv_nsec / 1000000), name, ev.type, ev.code, ev.value);
		if (n > 0) {
			ssize_t w = write(lfd, buf, (size_t)n);
			(void)w;
		}
	}
}

// Map an evdev KEY code to a Godot key (+ optional joypad button).
// Trimui Brick (firmware uinput gamepad): A=BTN_SOUTH, B=BTN_EAST,
// X=BTN_WEST, Y=BTN_NORTH, START=BTN_START, SELECT=BTN_SELECT,
// L1=BTN_TL, R1=BTN_TR, dpad=ABS_HAT0X/Y.
static bool fbdev_evdev_map_key(int p_code, Key &r_key, int &r_joypad_btn) {
	r_joypad_btn = -1;
	switch (p_code) {
		case BTN_SOUTH: // physical A
			r_key = Key::ENTER;
			r_joypad_btn = (int)JoyButton::A;
			return true;
		case BTN_EAST: // physical B
			r_key = Key::ESCAPE;
			r_joypad_btn = (int)JoyButton::B;
			return true;
		case BTN_NORTH: // physical Y
			r_key = Key::E;
			r_joypad_btn = (int)JoyButton::Y;
			return true;
		case BTN_WEST: // physical X
			r_key = Key::SPACE;
			r_joypad_btn = (int)JoyButton::X;
			return true;
		case BTN_START:
			r_key = Key::F1;
			r_joypad_btn = (int)JoyButton::START;
			return true;
		case BTN_SELECT:
			r_key = Key::F2;
			r_joypad_btn = (int)JoyButton::BACK;
			return true;
		case BTN_TL: // L1
			r_key = Key::Q;
			r_joypad_btn = (int)JoyButton::LEFT_SHOULDER;
			return true;
		case BTN_TR: // R1
			r_key = Key::W;
			r_joypad_btn = (int)JoyButton::RIGHT_SHOULDER;
			return true;
		case BTN_TL2: // L2
			r_key = Key::F4;
			r_joypad_btn = (int)/* triggers: no JoyButton slot */ -1;
			return true;
		case BTN_TR2: // R2
			r_key = Key::F5;
			r_joypad_btn = (int)/* triggers: no JoyButton slot */ -1;
			return true;
		case BTN_THUMBL:
			r_key = Key::F6;
			r_joypad_btn = (int)JoyButton::LEFT_STICK;
			return true;
		case BTN_THUMBR:
			r_key = Key::F7;
			r_joypad_btn = (int)JoyButton::RIGHT_STICK;
			return true;
		case KEY_UP:
			r_key = Key::UP;
			return true;
		case KEY_DOWN:
			r_key = Key::DOWN;
			return true;
		case KEY_LEFT:
			r_key = Key::LEFT;
			return true;
		case KEY_RIGHT:
			r_key = Key::RIGHT;
			return true;
		default:
			return false;
	}
}

} // namespace

void DisplayServerFbdev::register_fbdev_driver() {
	register_create_function("fbdev", create_func, get_rendering_drivers_func);
	fbdev_diag("[reg] fbdev driver registered\n");
}

DisplayServer *DisplayServerFbdev::create_func(const String &p_rendering_driver, DisplayServer::WindowMode p_mode, DisplayServer::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, Context p_context, int64_t p_parent_window, Error &r_error) {
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "[create] rendering_driver='%s'\n", p_rendering_driver.utf8().get_data());
		fbdev_diag(buf);
	}
	if (p_rendering_driver != "opengl3_es" && p_rendering_driver != "opengl3") {
		fbdev_diag("[create] REJECTED: unexpected rendering driver\n");
		r_error = ERR_UNAVAILABLE;
		return nullptr;
	}

	DisplayServerFbdev *server = memnew(DisplayServerFbdev);
	if (server->init_fbdev() != OK) {
		fbdev_diag("[create] init_fbdev FAILED\n");
		memdelete(server);
		r_error = ERR_CANT_CREATE;
		return nullptr;
	}

	RasterizerGLES3::make_current(false); // GLES mode, not GLES-over-GL.
	fbdev_diag("[create] SUCCESS (gles make_current done)\n");
	r_error = OK;
	return server;
}

DisplayServerFbdev::DisplayServerFbdev() {
	native_menu = memnew(NativeMenu);
	if (const char *bt = getenv("GODOT_FBDEV_BT")) {
		if (bt[0] == '1') {
		signal(SIGUSR1, fbdev_bt_handler);
		}
	}
	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);
	fbdev_diagf("[evdev] gates: key=%d joy=%d\n", (int)fbdev_evkey_on(), (int)fbdev_evjoy_on());
	// Events pushed before the main loop starts are buffered by Input.
	evdev_thread.start(&DisplayServerFbdev::_evdev_thread_trampoline, this);
}

DisplayServerFbdev::~DisplayServerFbdev() {
	evdev_stop = true;
	if (evdev_thread.is_started()) {
		evdev_thread.wait_to_finish();
	}
	for (EvdevDev &d : evdev_devs) {
		if (d.fd >= 0) {
			::close(d.fd);
		}
	}
	if (egl_ok) {
		eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (egl_surface != EGL_NO_SURFACE) {
			eglDestroySurface(egl_display, egl_surface);
		}
		if (egl_context != EGL_NO_CONTEXT) {
			eglDestroyContext(egl_display, egl_context);
		}
		eglTerminate(egl_display);
	}
	if (fb_mem) {
		munmap(fb_mem, fb_map_size);
	}
	if (fb_fd >= 0) {
		close(fb_fd);
	}
	if (native_menu) {
		memdelete(native_menu);
		native_menu = nullptr;
	}
}

Error DisplayServerFbdev::_init_fb() {
	const char *fb_path = getenv("GODOT_FBDEV_FB");
	if (!fb_path) {
		fb_path = "/dev/fb0";
	}
	// Legacy glReadPixels->fb0 blit, diagnostics only. The real present
	// path is the vendor DC via the EGL window surface.
	const char *fb0_env = getenv("GODOT_FBDEV_FB0");
	const bool legacy_blit = fb0_env && fb0_env[0] != '0' && fb0_env[0] != '\0';
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "[init_fb] opening %s (legacy blit %s)\n", fb_path, legacy_blit ? "ON" : "off");
		fbdev_diag(buf);
	}

	fb_fd = open(fb_path, legacy_blit ? O_RDWR : O_RDONLY);
	if (fb_fd < 0) {
		char buf[160];
		snprintf(buf, sizeof(buf), "[init_fb] OPEN FAILED errno=%d (%s)\n", errno, strerror(errno));
		fbdev_diag(buf);
		ERR_PRINT(vformat("fbdev: cannot open %s: %s", fb_path, strerror(errno)));
		return ERR_FILE_CANT_OPEN;
	}
	fbdev_diag("[init_fb] opened, querying info\n");

	struct fb_var_screeninfo var;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0) {
		ERR_PRINT("fbdev: FBIOGET_VSCREENINFO failed");
		return ERR_CANT_CREATE;
	}
	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0) {
		ERR_PRINT("fbdev: FBIOGET_FSCREENINFO failed");
		return ERR_CANT_CREATE;
	}

	fb_width = var.xres;
	fb_height = var.yres;
	fb_stride = (var.xres_virtual * var.bits_per_pixel) / 8;
	if (fb_stride < fb_width * 4) {
		fb_stride = fb_width * 4;
	}

	if (legacy_blit) {
		size_t map_size = fix.smem_len;
		if (map_size == 0) {
			map_size = (size_t)var.xres_virtual * var.yres_virtual * var.bits_per_pixel / 8;
		}

		fb_mem = (uint8_t *)mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
		if (fb_mem == MAP_FAILED) {
			fb_mem = nullptr;
			fbdev_diag("[init_fb] MMAP FAILED\n");
			ERR_PRINT("fbdev: mmap of framebuffer failed");
			return ERR_CANT_CREATE;
		}
		fb_map_size = map_size;
		fbdev_diag("[init_fb] mmap OK (legacy blit)\n");
	}

	print_line(vformat("fbdev: %s %dx%d bpp=%d (panel geometry)",
			fb_path, (int)fb_width, (int)fb_height, (int)var.bits_per_pixel));

	if (var.bits_per_pixel != 32) {
		WARN_PRINT(vformat("fbdev: unexpected %u bpp framebuffer; the legacy blit assumes 32bpp.", var.bits_per_pixel));
	}

	return OK;
}

bool DisplayServerFbdev::_init_egl() {
	// glad loader: dlopen libEGL.so.1, bind the core, then reload against a
	// live display to pick up extension entry points (same dance as EGLManager).
	fbdev_diag("[init_egl] gladLoaderLoadEGL\n");
	if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) {
		fbdev_diag("[init_egl] LOAD FAILED\n");
		ERR_PRINT("fbdev: can't load EGL dynamic library");
		return false;
	}

	egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (egl_display == EGL_NO_DISPLAY) {
		fbdev_diag("[init_egl] GETDISPLAY FAILED\n");
		ERR_PRINT("fbdev: eglGetDisplay failed");
		return false;
	}

	EGLint maj = 0, min = 0;
	if (!eglInitialize(egl_display, &maj, &min)) {
		fbdev_diag("[init_egl] INITIALIZE FAILED\n");
		ERR_PRINT("fbdev: eglInitialize failed");
		return false;
	}
	gladLoaderLoadEGL(egl_display);
	fbdev_diag("[init_egl] display up, choosing config\n");

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config = nullptr;
	EGLint config_count = 0;
	eglChooseConfig(egl_display, config_attribs, &config, 1, &config_count);
	if (config_count == 0) {
		// Some vendor configs advertise the window/pbuffer bits together.
		EGLint fallback_attribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_NONE,
		};
		eglChooseConfig(egl_display, fallback_attribs, &config, 1, &config_count);
	}
	if (config_count == 0) {
		fbdev_diag_egl_err("init_egl CHOOSECONFIG");
		ERR_PRINT("fbdev: no window-surface/ES3 EGL config available");
		return false;
	}
	fbdev_diag("[init_egl] config ok, creating context\n");

	EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs);
	if (egl_context == EGL_NO_CONTEXT) {
		fbdev_diag_egl_err("init_egl CREATECONTEXT");
		ERR_PRINT("fbdev: eglCreateContext(ES3) failed");
		return false;
	}
	fbdev_diag("[init_egl] context ok, creating window surface\n");

	// The vendor NULL-window-system backend (WSEGL_CreateWindowDrawable,
	// "MALI_CreateWindow") allocates the display-controller buffer here;
	// eglSwapBuffers() later presents it to the panel.
	egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)0, nullptr);
	if (egl_surface == EGL_NO_SURFACE) {
		fbdev_diag_egl_err("init_egl CREATEWINDOWSURFACE");
		ERR_PRINT("fbdev: eglCreateWindowSurface failed");
		return false;
	}
	fbdev_diag("[init_egl] WINDOW SURFACE OK (DC buffer claimed)\n");

	// Prefer the real surface size when the vendor reports it.
	EGLint sw = 0, sh = 0;
	eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &sw);
	eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &sh);
	if (sw > 0 && sh > 0) {
		fb_width = (uint32_t)sw;
		fb_height = (uint32_t)sh;
		fbdev_diag("[init_egl] surface size from eglQuerySurface\n");
	}

	// The context must be current before the GLES3 rasterizer's constructor
	// runs its GLAD load (it calls glGetString(GL_VERSION), which needs a
	// live context). The render thread will take the context over later via
	// gl_window_make_current().
	if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
		fbdev_diag_egl_err("init_egl MAKECURRENT");
		ERR_PRINT("fbdev: eglMakeCurrent failed");
		return false;
	}
	fbdev_diag("[init_egl] context made current\n");

	print_line(vformat("fbdev: EGL %d.%d, window surface %dx%d ES3 context ready (DC present)", maj, min, (int)fb_width, (int)fb_height));
	egl_ok = true;
	return true;
}

Error DisplayServerFbdev::init_fbdev() {
	Error err = _init_fb();
	if (err != OK) {
		return err;
	}
	if (!_init_egl()) {
		return ERR_CANT_CREATE;
	}
	return OK;
}

void DisplayServerFbdev::gl_window_make_current(DisplayServer::WindowID p_window_id) {
	if (egl_ok) {
		eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
	}
}

void DisplayServerFbdev::swap_buffers() {
	static bool first_swap_logged = false;
	if (!first_swap_logged) {
		first_swap_logged = true;
		fbdev_diag("[swap] first swap_buffers (DC present via eglSwapBuffers)\n");
	}
	if (!egl_ok) {
		return;
	}

	// The scene may be left bound to an intermediate FBO; present the
	// window surface's default framebuffer.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Present to the panel through the vendor display controller.
	eglSwapBuffers(egl_display, egl_surface);

	// Legacy diagnostics path: also blit the frame into /dev/fb0.
	if (fb_mem) {
		const size_t row_bytes = (size_t)fb_width * 4;
		const size_t want = row_bytes * (size_t)fb_height;
		if ((size_t)readback.size() != want) {
			readback.resize(want);
		}

		// BGRA keeps the byte order matching a little-endian 0xAARRGGBB
		// framebuffer, so the blit is a plain copy.
		glReadPixels(0, 0, fb_width, fb_height, GL_BGRA, GL_UNSIGNED_BYTE, readback.ptrw());

		// GL row 0 is the bottom of the image; the framebuffer row 0 is
		// the top, so blit with rows flipped.
		const uint8_t *src = readback.ptr();
		for (uint32_t y = 0; y < fb_height; y++) {
			memcpy(fb_mem + (fb_height - 1 - y) * fb_stride, src + y * row_bytes, row_bytes);
		}
	}

	// The vendor swap is not vsynced, so pace ourselves here. The target
	// can be overridden (e.g. 30) via GODOT_FBDEV_FPS.
	const char *fps_env = getenv("GODOT_FBDEV_FPS");
	uint64_t target_fps = fps_env ? strtoull(fps_env, nullptr, 10) : 60;
	if (target_fps == 0) {
		target_fps = 60;
	}
	const uint64_t frame_budget_usec = 1000000 / target_fps;
	uint64_t now = OS::get_singleton()->get_ticks_usec();
	uint64_t elapsed = now - last_swap_usec;
	if (elapsed < frame_budget_usec) {
		usleep((useconds_t)(frame_budget_usec - elapsed));
	}
	last_swap_usec = OS::get_singleton()->get_ticks_usec();
}

void DisplayServerFbdev::_evdev_thread_trampoline(void *p_user) {
	static_cast<DisplayServerFbdev *>(p_user)->evdev_thread_main();
}

// Logical pressed-state lookup (evdev thread only). p_is_joy namespaces the
// id space (keys use the Key value, joypad buttons use 1'000'000 + button).
bool DisplayServerFbdev::evdev_dedup_state(int64_t p_id, bool p_is_joy, bool p_pressed, bool p_echo, bool &r_allow) {
	for (EvdevDedup &e : evdev_dedup) {
		if (e.id == p_id) {
			if (p_pressed) {
				// Suppress repeats and duplicate presses while held
				// (mirrored devices / key repeat).
				r_allow = !e.pressed && !p_echo;
				e.pressed = true;
			} else {
				// Drop stale releases (device A releases while mirrored
				// device B still reports pressed -> logical state stays).
				r_allow = e.pressed;
				e.pressed = false;
			}
			return true;
		}
	}
	if (p_pressed && !p_echo) {
		evdev_dedup.push_back(EvdevDedup{ p_id, true });
	}
	r_allow = p_pressed && !p_echo;
	return false;
}

void DisplayServerFbdev::evdev_push_key(Key p_key, int p_joypad_btn, bool p_pressed, bool p_echo) {
	bool allow_key = false, allow_joy = false;
	const bool has_key = p_key != Key::NONE;
	if (has_key) {
		evdev_dedup_state((int64_t)p_key, false, p_pressed, p_echo, allow_key);
	}
	if (p_joypad_btn >= 0) {
		evdev_dedup_state(1000000 + p_joypad_btn, true, p_pressed, p_echo, allow_joy);
	}
	Ref<InputEvent> key_ev;
	if (has_key && allow_key && fbdev_evkey_on()) {
		Ref<InputEventKey> k;
		k.instantiate();
		k->set_keycode(p_key);
		k->set_physical_keycode(p_key); // no hardware scancodes; logical code
		k->set_key_label(p_key);
		k->set_pressed(p_pressed);
		k->set_echo(p_echo);
		// Keep modifier state consistent so is_action() works on releases.
		if (p_key == Key::SHIFT) k->set_shift_pressed(p_pressed);
		if (p_key == Key::CTRL) k->set_ctrl_pressed(p_pressed);
		if (p_key == Key::ALT) k->set_alt_pressed(p_pressed);
		if (p_key == Key::META) k->set_meta_pressed(p_pressed);
		key_ev = k;
	}
	Ref<InputEvent> joy_ev;
	if (p_joypad_btn >= 0 && allow_joy && fbdev_evjoy_on()) {
		Ref<InputEventJoypadButton> jb;
		jb.instantiate();
		jb->set_button_index((JoyButton)p_joypad_btn);
		jb->set_pressed(p_pressed);
		joy_ev = jb;
	}
	uint64_t pushed = 0;
	{
		MutexLock lock(evdev_queue_mutex);
		if (key_ev.is_valid()) {
			evdev_queue.push_back(key_ev);
			pushed++;
		}
		if (joy_ev.is_valid()) {
			evdev_queue.push_back(joy_ev);
			pushed++;
		}
	}
	if (pushed) {
		evdev_push_count.fetch_add(pushed, std::memory_order_relaxed);
	}
}

void DisplayServerFbdev::process_events() {
	// Main thread, once per iteration: deliver queued evdev events to
	// Input (same pattern as the X11/Wayland display servers' OS event
	// queues), and handle the DS-level quit fallback.
	Vector<Ref<InputEvent>> pending;
	bool quit_requested = false;
	{
		MutexLock lock(evdev_queue_mutex);
		pending = evdev_queue;
		evdev_queue.clear();
		quit_requested = evdev_quit_requested;
		evdev_quit_requested = false;
	}
	for (const Ref<InputEvent> &e : pending) {
		Input::get_singleton()->parse_input_event(e);
	}
	if (quit_requested) {
		fbdev_diag("[evdev] START+SELECT -> quit\n");
		MainLoop *ml = OS::get_singleton()->get_main_loop();
		if (ml) {
			if (SceneTree *st = Object::cast_to<SceneTree>(ml)) {
				st->quit();
			}
		}
	}
	Input::get_singleton()->flush_buffered_events();
	{
		static uint64_t report_frame = 0;
		if ((report_frame++ % 600) == 0) {
			fbdev_diagf("[evdev] report frame=%llu push_total=%llu pending=%d\n",
				(unsigned long long)report_frame,
				(unsigned long long)evdev_push_count.load(std::memory_order_relaxed),
				(int)evdev_queue.size());
		}
	}
}

void DisplayServerFbdev::evdev_thread_main() {
	// Discover input devices (firmware uinput devices appear here too).
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		fbdev_diag("[evdev] /dev/input missing\n");
		return;
	}
	while (true) {
		struct dirent *e = readdir(dir);
		if (!e) {
			break;
		}
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}
		String dev_path = String("/dev/input/") + e->d_name;
		const char *path = dev_path.utf8().get_data();
		int fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0) {
			fd = open(path, O_RDONLY | O_NONBLOCK);
		}
		if (fd < 0) {
			char line[160];
			snprintf(line, sizeof(line), "[evdev] open %s FAILED errno=%d\n", e->d_name, errno);
			fbdev_diag(line);
			continue;
		}
		// Only devices that can emit key or axis events. Buffer sizes match
		// the kernel masks (KEY_MAX=0x2ff -> 12 longs, ABS_MAX -> 8 longs).
		unsigned long kbits[12] = { 0 }, abits[8] = { 0 };
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(kbits)), kbits);
		bool has_keys = false;
		for (int w = 0; w < 12; w++) {
			if (kbits[w]) {
				has_keys = true;
			}
		}
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abits)), abits);
		bool has_abs = false;
		for (int w = 0; w < 8; w++) {
			if (abits[w]) {
				has_abs = true;
			}
		}
		if (!has_keys && !has_abs) {
			char line[96];
			snprintf(line, sizeof(line), "[evdev] skip %s (no key/abs)\n", e->d_name);
			fbdev_diag(line);
			close(fd);
			continue;
		}
		EvdevDev dev;
		dev.fd = fd;
		ioctl(fd, EVIOCGNAME(sizeof(dev.name) - 1), dev.name);
		evdev_devs.push_back(dev);
		char line[96];
		snprintf(line, sizeof(line), "[evdev] opened %s (%s)\n", e->d_name, dev.name);
		fbdev_diag(line);
	}
	closedir(dir);

	if (evdev_devs.is_empty()) {
		fbdev_diag("[evdev] no usable devices\n");
		return;
	}

	Vector<struct pollfd> pfds;
	for (const EvdevDev &d : evdev_devs) {
		struct pollfd p;
		p.fd = d.fd;
		p.events = POLLIN;
		pfds.push_back(p);
	}

	while (!evdev_stop) {
		poll(pfds.ptrw(), (nfds_t)pfds.size(), 100);
		for (int i = 0; i < pfds.size(); i++) {
			if (!(pfds[i].revents & POLLIN)) {
				continue;
			}
			EvdevDev &dev = evdev_devs.write[i];
			struct input_event ev;
			while (read(dev.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (evdev_stop) {
					break;
				}
				if (ev.type == EV_SYN) {
					continue;
				}
				fbdev_evdev_log(dev.name, ev);

				if (ev.type == EV_KEY) {
					bool pressed = ev.value != 0;
					bool repeat = ev.value == 2;
					Key key;
					int jbtn = -1;
					if (fbdev_evdev_map_key(ev.code, key, jbtn)) {
						evdev_push_key(key, jbtn, pressed, repeat);
					}
					// DS-level quit fallback: START + SELECT together.
					if (ev.code == BTN_START) {
						dev.start_held = pressed;
					}
					if (ev.code == BTN_SELECT) {
						dev.select_held = pressed;
					}
					if (dev.start_held && dev.select_held) {
						// Flag only; process_events() acts on it (main thread).
						MutexLock lock(evdev_queue_mutex);
						evdev_quit_requested = true;
					}
				} else if (ev.type == EV_ABS) {
					// D-pad / axis sign changes -> directional edges. SDL model:
					// a gamepad hat is a GAMEPAD channel — synthesize joypad
					// D-pad buttons only (no arrow-key synthesis from the hat;
					// real arrow keys come from keyboard-classified devices).
					// State is GLOBAL (not per-device) because firmware input
					// layers can mirror the same physical D-pad through several
					// event devices; one logical edge = one press. evdev_push_key
					// additionally de-dups the logical pressed state.
					auto axis_edge = [&](int &state, int val, Key neg_key, int neg_jbtn, Key pos_key, int pos_jbtn) {
						int dir = val > 0 ? 1 : (val < 0 ? -1 : 0);
						if (dir != state) {
							if (state != 0) {
								evdev_push_key(state < 0 ? neg_key : pos_key, state < 0 ? neg_jbtn : pos_jbtn, false, false);
							}
							if (dir != 0) {
								evdev_push_key(dir < 0 ? neg_key : pos_key, dir < 0 ? neg_jbtn : pos_jbtn, true, false);
							}
							state = dir;
						}
					};
					switch (ev.code) {
						case ABS_HAT0X:
							axis_edge(evdev_hat_x, ev.value, Key::NONE, (int)JoyButton::DPAD_LEFT, Key::NONE, (int)JoyButton::DPAD_RIGHT);
							break;
						case ABS_HAT0Y:
							axis_edge(evdev_hat_y, ev.value, Key::NONE, (int)JoyButton::DPAD_UP, Key::NONE, (int)JoyButton::DPAD_DOWN);
							break;
						case ABS_X:
							axis_edge(dev.abs_x, ev.value, Key::LEFT, -1, Key::RIGHT, -1);
							break;
						case ABS_Y:
							axis_edge(dev.abs_y, ev.value, Key::UP, -1, Key::DOWN, -1);
							break;
						default:
							break;
					}
				}
			}
		}
	}
}

#endif // FBDEV_ENABLED

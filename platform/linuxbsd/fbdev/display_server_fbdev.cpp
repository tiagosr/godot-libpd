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

#include "core/os/os.h"
#include "drivers/gles3/rasterizer_gles3.h"
#include "platform_gl.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {
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

void fbdev_diag_egl_err(const char *what) {
	char buf[160];
	snprintf(buf, sizeof(buf), "[%s] FAILED (eglError=0x%04x)\n", what, eglGetError());
	fbdev_diag(buf);
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
	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);
}

DisplayServerFbdev::~DisplayServerFbdev() {
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

#endif // FBDEV_ENABLED

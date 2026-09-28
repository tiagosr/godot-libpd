/* winstest.c — eglCreateWindowSurface (NULL window) + glClear + eglSwapBuffers.
 * Hypothesis: on this vendor stack (libpvrNULL_WSEGL), the WindowSurface is
 * what claims the PVRSRVDC display buffer (WSEGL_CreateWindowDrawable,
 * logs "MALI_CreateWindow"), and only that path presents to the LCD.
 * Build: gcc -O2 -o winstest winstest.c -lEGL -lGLESv2
 * Run:   ./winstest [seconds, default 20]
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv)
{
	/* real-time pacing: ~5 color changes/sec, true elapsed seconds */
	int secs = (argc > 1) ? atoi(argv[1]) : 25;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) { printf("NO_DEFAULT_DISPLAY\n"); return 2; }
	EGLint maj = 0, min = 0;
	if (!eglInitialize(dpy, &maj, &min)) {
		printf("INIT_FAIL (eglError=0x%x)\n", eglGetError());
		return 3;
	}
	printf("winstest: EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

	EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
		printf("NO_WINDOW_ES3_CONFIG ncfg=%d (eglError=0x%x); trying PBuffer-bit config\n", ncfg, eglGetError());
		EGLint cfg2[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_NONE
		};
		if (!eglChooseConfig(dpy, cfg2, &cfg, 1, &ncfg) || ncfg < 1) {
			printf("NO_CONFIG_AT_ALL (eglError=0x%x)\n", eglGetError());
			return 4;
		}
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) { printf("CTX_FAIL (eglError=0x%x)\n", eglGetError()); return 5; }

	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
	if (surf == EGL_NO_SURFACE) {
		printf("WINDOWSURFACE_FAIL (eglError=0x%x)\n", eglGetError());
		return 6;
	}
	printf("winstest: WindowSurface created (expect MALI_CreateWindow log from vendor)\n");
	fflush(stdout);
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("MAKECURRENT_FAIL (eglError=0x%x)\n", eglGetError()); return 7; }
	printf("winstest: current; running %ds of eglSwapBuffers\n", secs);
	fflush(stdout);

	const float cols[4][3] = { {1.0f, 0.1f, 0.1f}, {0.1f, 1.0f, 0.1f}, {0.1f, 0.1f, 1.0f}, {0.9f, 0.9f, 0.9f} };
	const char *names[4] = { "RED", "GREEN", "BLUE", "GRAY" };
	/* real-time pacing: run for `secs` real (wall) seconds at 5 fps, new color every 5s */
	long start_s = time(NULL);
	for (;;) {
		long elapsed_s = time(NULL) - start_s;
		if (elapsed_s >= secs) break;
		int ci = (elapsed_s / 5) % 4;
		glClearColor(cols[ci][0], cols[ci][1], cols[ci][2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		EGLBoolean ok = eglSwapBuffers(dpy, surf);
		if (elapsed_s % 5 == 0)
			printf("t=%2ds %s swap=%d err=0x%x\n", elapsed_s, names[ci], (int)ok, eglGetError());
		fflush(stdout);
		usleep(200000); /* 5 fps */
	}
	printf("winstest: done\n");
	return 0;
}

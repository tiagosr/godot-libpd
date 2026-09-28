/* swaptest.c — EGL PBuffer + glClear + eglSwapBuffers, NO /dev/fb0.
 * Tests whether eglSwapBuffers() on this vendor stack
 * (libEGL shim -> libIMGegl + libpvrNULL_WSEGL, PVRSRVDC display path)
 * is the actual "present to LCD" call.
 * Style mirrors egl_probe.c (direct symbol linking).
 * Build: gcc -O2 -o swaptest swaptest.c -lEGL -lGLESv2 (vendor libs by path)
 * Run:   ./swaptest [seconds, default 20]
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = (argc > 1) ? atoi(argv[1]) : 20;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) { printf("NO_DEFAULT_DISPLAY\n"); return 2; }
	EGLint maj = 0, min = 0;
	if (!eglInitialize(dpy, &maj, &min)) {
		printf("INIT_FAIL (eglError=0x%x)\n", eglGetError());
		return 3;
	}
	printf("swaptest: EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

	EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
		printf("NO_PBUFFER_ES3_CONFIG ncfg=%d (eglError=0x%x)\n", ncfg, eglGetError());
		return 4;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) { printf("CTX_FAIL (eglError=0x%x)\n", eglGetError()); return 5; }
	EGLint pb_attr[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 768, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
	if (surf == EGL_NO_SURFACE) { printf("PBUFFER_FAIL (eglError=0x%x)\n", eglGetError()); return 6; }
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("MAKECURRENT_FAIL (eglError=0x%x)\n", eglGetError()); return 7; }

	printf("swaptest: PBuffer 1024x768 ES3 current; %ds of eglSwapBuffers (no fb0)\n", secs);
	fflush(stdout);

	int t = 0;
	while (t < secs) {
		const float cols[4][3] = { {1.0f, 0.1f, 0.1f}, {0.1f, 1.0f, 0.1f}, {0.1f, 0.1f, 1.0f}, {0.9f, 0.9f, 0.9f} };
		const char *names[4] = { "RED", "GREEN", "BLUE", "GRAY" };
		for (int i = 0; i < 4 && t < secs; i++) {
			glClearColor(cols[i][0], cols[i][1], cols[i][2], 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			EGLBoolean ok = eglSwapBuffers(dpy, surf);
			printf("t=%2d %s swap=%d err=0x%x\n", t, names[i], (int)ok, eglGetError());
			fflush(stdout);
			t += 2;
			usleep(200000);
		}
	}
	printf("swaptest: done\n");
	return 0;
}

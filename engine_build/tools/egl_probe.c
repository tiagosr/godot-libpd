/* egl_probe.c — EGL/GLES capability probe for the device GPU.
 * Answers: GLES version, renderer, and PBuffer surface support
 * (PBuffer = the surface type the fbdev DisplayServer will render into).
 * Links by filename (libEGL.so / libGLESv2.so) to bind whatever blob the
 * device ships, whatever its soname is.
 */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <string.h>

#define CHKPTR(x) do { void *_p = (void *)(x); if (!_p) { \
    printf("FAIL: %s (eglError=0x%x)\n", #x, eglGetError()); return 10; } } while (0)
#define CHKEGL(x) do { if (!(x)) { \
    printf("FAIL: %s (eglError=0x%x)\n", #x, eglGetError()); return 10; } } while (0)

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { printf("NO_DEFAULT_DISPLAY\n"); return 2; }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) {
        printf("INIT_FAIL (eglError=0x%x)\n", eglGetError());
        return 3;
    }
    printf("EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
    printf("EGL_VERSION=%s\n", eglQueryString(dpy, EGL_VERSION));
    printf("EGL_EXTENSIONS=%s\n\n", eglQueryString(dpy, EGL_EXTENSIONS));

    /* PBuffer + ES3 config */
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
    if (ncfg == 0) {
        printf("NO_PBUFFER_ES3_CONFIG (eglError=0x%x)\n", eglGetError());
        EGLint cfg2[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE
        };
        eglChooseConfig(dpy, cfg2, &cfg, 1, &ncfg);
        printf("retry-noalpha: ncfg=%d\n", ncfg);
        if (ncfg == 0) return 4;
    }
    EGLint rsz = 0, db = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_RED_SIZE, &rsz);
    eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &db);
    printf("PBUFFER CONFIG ok (red_size=%d depth=%d)\n", rsz, db);

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    CHKPTR(ctx);
    EGLint pb_attr[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 768, EGL_NONE };
    EGLSurface sb = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    CHKPTR(sb);
    CHKEGL(eglMakeCurrent(dpy, sb, sb, ctx));

    printf("\nGL_VERSION=%s\n", glGetString(GL_VERSION));
    printf("GL_VENDOR=%s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
    printf("GL_SHADING_LANGUAGE_VERSION=%s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* quick render smoke: full-screen triangle */
    static const char *vs =
        "#version 300 es\nlayout(location=0) in vec2 p;\nvoid main(){gl_Position=vec4(p,0.,1.);}";
    static const char *fs =
        "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main(){c=vec4(0.,0.6,1.,1.);}";
    GLuint prog = glCreateProgram();
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    GLchar log[512]; GLint ok;
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(v, 512, NULL, log); printf("VS FAIL: %s\n", log); return 5; }
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(f, 512, NULL, log); printf("FS FAIL: %s\n", log); return 5; }
    glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(prog, 512, NULL, log); printf("LINK FAIL: %s\n", log); return 6; }
    float verts[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    GLuint vao = 0, vb = 0;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vb); glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(0, 0, 1024, 768);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLubyte px[4] = {0, 0, 0, 0};
    glReadPixels(512, 384, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("RENDER_OK center_pixel=%d,%d,%d,%d (expect ~0,153,255,255)\n",
           px[0], px[1], px[2], px[3]);
    return 0;
}

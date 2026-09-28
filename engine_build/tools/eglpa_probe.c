/* eglpa_probe.c — test eglGetProcAddress resolution of core GLES3 entry points. */
#include <stdio.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

int main(void) {
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL); }
    if (!egl) { printf("NO_LIBEGL\n"); return 2; }
    PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
    if (!gpa) { printf("NO_eglGetProcAddress\n"); return 3; }
    const char *names[] = {
        "glGetString", "glCreateProgram", "glCreateShader", "glLinkProgram",
        "glUseProgram", "glCreateBuffer", "glBindBuffer", "glBufferData",
        "glVertexAttribPointer", "glDrawArrays", "glViewport", "glClearColor",
        "glClear", "glReadPixels", "glGenFramebuffers", "glBindFramebuffer",
        "glGenVertexArrays", "glBindVertexArray", "glEnableVertexAttribArray",
        "glDeleteShader", "glAttachShader", "glCompileShader", "glShaderSource",
        "glGetShaderiv", "glGetProgramiv", "glActiveTexture", "glUniform1i",
        "glGenTextures", "glBindTexture", "glTexImage2D", "glTexParameteri",
        "glDeleteProgram", "glDeleteBuffers", "glDeleteFramebuffers",
        "glDeleteVertexArrays", "glDeleteTextures", "glGenRenderbuffers",
        "glBindRenderbuffer", "glFramebufferTexture2D", "glCheckFramebufferStatus",
        "glBlitFramebuffer", "glGenerateMipmap", "glFlush", "glFinish", NULL
    };
    int ok = 0, fail = 0;
    for (int i = 0; names[i]; i++) {
        void *p = gpa(names[i]);
        if (p) ok++;
        else { fail++; printf("NULL: %s\n", names[i]); }
    }
    printf("RESOLVED %d, NULL %d\n", ok, fail);
    return fail ? 1 : 0;
}

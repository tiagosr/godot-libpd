// Android bionic compatibility: glob(3)/globfree(3) were added to bionic in
// API 28. libpd links them from pure-data's x_file.c ([file glob] object,
// search-path globbing). When targeting < API 28 the symbols do not exist in
// libc, so provide link-compatible no-ops that report "no match".
//
// godot-libpd is unaffected in v1: patch paths are resolved on the Godot side
// (LibpdInstance::resolve_patch_path) before libpd_openfile() is called, so
// pd's search-path globbing is never exercised. On API 28+ devices the real
// bionic implementation is used (the stub compiles to nothing).
#include <glob.h>
#include <stddef.h>

#if defined(__ANDROID__) && (__ANDROID_API__ < 28)

int glob(const char *p_pattern, int p_flags,
		int (*p_errfunc)(const char *, int), glob_t *p_result) {
	(void)p_pattern;
	(void)p_flags;
	(void)p_errfunc;
	if (p_result != NULL) {
		p_result->gl_pathc = 0;
		p_result->gl_matchc = 0;
		p_result->gl_pathv = NULL;
	}
	return GLOB_NOMATCH;
}

void globfree(glob_t *p_result) {
	(void)p_result;
}

#endif

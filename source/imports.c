/* imports.c -- libProject_Douglas_HH.so import resolution (TFA)
 *
 * Binds every one of the 218 undefined dynamic symbols of TFA's
 * libProject_Douglas_HH.so. GL goes straight to the native mesa/nouveau
 * GLESv2 (the wrapper owns EGL, so only a handful of egl* calls are wrapped),
 * audio to the vendored OpenSL ES, threads/fs/locale through the bionic shims,
 * and the rest to newlib. See _re/specs/B-imports.md for the full derivation.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "pthr.h"
#include "hooks.h"

// crt/newlib-provided symbols forwarded by address (declared as data so we can
// take &sym to get the function address the dynamic loader needs).
// __errno is declared by <errno.h> as `int *__errno(void)`; we bind &__errno.
extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

// ---------------------------------------------------------------------------
// small local shims
// ---------------------------------------------------------------------------

// sleep family: park GL ownership before blocking (no-ops in this port, but
// kept symmetric with the LSWTCS structure). Implemented via nanosleep so they
// never recurse into the usleep/sleep imports they replace.
static unsigned usleep_park(useconds_t usec) {
  egl_gl_ownership_park();
  struct timespec ts = { (time_t)(usec / 1000000), (long)(usec % 1000000) * 1000 };
  nanosleep(&ts, NULL);
  egl_gl_service_handover();
  return 0;
}

static unsigned sleep_park(unsigned sec) {
  egl_gl_ownership_park();
  struct timespec ts = { (time_t)sec, 0 };
  nanosleep(&ts, NULL);
  egl_gl_service_handover();
  return 0;
}

// logged abort so debug.log shows when the game bails via abort()/assert
// (these exit cleanly and don't produce an Atmosphere crash report)
static void abort_log(void) {
  debugPrintf("!!! game called abort()\n");
  abort();
}

// logged wrapper so debug.log shows the game's audio engine bring-up
static SLresult slCreateEngineHook(SLObjectItf *pEngine, SLuint32 numOptions,
                                   const SLEngineOption *pEngineOptions,
                                   SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                                   const SLboolean *pInterfaceRequired) {
  debugPrintf("SL: slCreateEngine(numOptions=%u, numInterfaces=%u)\n",
              (unsigned)numOptions, (unsigned)numInterfaces);
  SLresult r = slCreateEngine(pEngine, numOptions, pEngineOptions,
                              numInterfaces, pInterfaceIds, pInterfaceRequired);
  debugPrintf("SL: slCreateEngine -> 0x%x\n", (unsigned)r);
  return r;
}

// ---------------------------------------------------------------------------
// v2.2.1.06 added imports: bionic _FORTIFY_SOURCE checked libc variants. The
// trailing dst-size guard arg(s) are ignored; the leading args match the plain
// function. (The old engine statically linked these; the new one imports them.)
// ---------------------------------------------------------------------------
static void  *memcpy_chk_fake(void *d, const void *s, size_t n, size_t dl){(void)dl;return memcpy(d,s,n);}
static void  *memmove_chk_fake(void *d, const void *s, size_t n, size_t dl){(void)dl;return memmove(d,s,n);}
static void  *memset_chk_fake(void *d, int c, size_t n, size_t dl){(void)dl;return memset(d,c,n);}
static char  *strcpy_chk_fake(char *d, const char *s, size_t dl){(void)dl;return strcpy(d,s);}
static char  *strncpy_chk_fake(char *d, const char *s, size_t n, size_t dl){(void)dl;return strncpy(d,s,n);}
static char  *strncpy_chk2_fake(char *d, const char *s, size_t n, size_t dl, size_t sl){(void)dl;(void)sl;return strncpy(d,s,n);}
static char  *strcat_chk_fake(char *d, const char *s, size_t dl){(void)dl;return strcat(d,s);}
static size_t strlen_chk_fake(const char *s, size_t ml){(void)ml;return strlen(s);}
static char  *strchr_chk_fake(const char *s, int c, size_t ml){(void)ml;return strchr(s,c);}
static char  *strrchr_chk_fake(const char *s, int c, size_t ml){(void)ml;return strrchr(s,c);}
static int    vsnprintf_chk_fake(char *d, size_t n, int fl, size_t dl, const char *fmt, va_list ap){(void)fl;(void)dl;return vsnprintf(d,n,fmt,ap);}
static int    vsprintf_chk_fake(char *d, int fl, size_t dl, const char *fmt, va_list ap){(void)fl;(void)dl;return vsprintf(d,fmt,ap);}

// __android_log_print -> route to debug.log; sched_yield -> yield the core;
// dl_iterate_phdr -> no shared objects (we're statically linked).
// (strerror_r_fake / syscall_fake / sysconf_fake already live in libc_shim.)
static int  android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; char buf[512]; va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  debugPrintf("[alog:%s] %s\n", tag ? tag : "?", buf); return 0;
}
static int  sched_yield_fake(void) { svcSleepThread(0); return 0; }
static int  dl_iterate_phdr_fake(void *cb, void *data) { (void)cb; (void)data; return 0; }

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- runtime / fortify ---------------------------------------------------
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },

  // --- AAsset (only the manager-from-java token is imported) ---------------
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },

  // --- math ----------------------------------------------------------------
  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "ceill", (uintptr_t)&ceill },
  { "cosf", (uintptr_t)&cosf },
  { "expf", (uintptr_t)&expf },
  { "floorl", (uintptr_t)&floorl },
  { "fmodl", (uintptr_t)&fmodl },
  { "log10f", (uintptr_t)&log10f },
  { "log10l", (uintptr_t)&log10l },
  { "logf", (uintptr_t)&logf },
  { "powf", (uintptr_t)&powf },
  { "powl", (uintptr_t)&powl },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sinf", (uintptr_t)&sinf },

  // --- memory / stdlib -----------------------------------------------------
  { "abort", (uintptr_t)&abort_log },
  { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi },
  { "bsearch", (uintptr_t)&bsearch },
  { "free", (uintptr_t)&free },
  { "malloc", (uintptr_t)&malloc },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "qsort", (uintptr_t)&qsort },
  { "realloc", (uintptr_t)&realloc },
  { "strtol", (uintptr_t)&strtol },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },

  // --- strings -------------------------------------------------------------
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strerror", (uintptr_t)&strerror },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtok", (uintptr_t)&strtok },

  // --- ctype ---------------------------------------------------------------
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "isgraph", (uintptr_t)&isgraph },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },

  // --- stdio ---------------------------------------------------------------
  { "fclose", (uintptr_t)&fclose_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgetc", (uintptr_t)&fgetc },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "rewind", (uintptr_t)&rewind },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },

  // --- filesystem ----------------------------------------------------------
  { "close", (uintptr_t)&close },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "open", (uintptr_t)&open_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "stat", (uintptr_t)&stat_fake },

  // --- syslog (no-op) ------------------------------------------------------
  { "closelog", (uintptr_t)&ret0 },
  { "openlog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  // --- time / sched / sleep ------------------------------------------------
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },
  { "sleep", (uintptr_t)&sleep_park },
  { "strftime", (uintptr_t)&strftime },
  { "usleep", (uintptr_t)&usleep_park },

  // --- EGL (wrapper owns display/ctx/surface; these query + tweak) ---------
  { "eglBindAPI", (uintptr_t)&eglBindAPIHook },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfigHook },
  { "eglCreateContext", (uintptr_t)&eglCreateContextHook },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurfaceHook },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddressHook },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrentHook },
  { "eglQuerySurface", (uintptr_t)&eglQuerySurfaceHook },
  { "eglSwapInterval", (uintptr_t)&eglSwapIntervalHook },

  // --- GLES2 (all serialized through hooks/egl.c so the render + loader -----
  //     threads never enter mesa-nouveau concurrently) ----------------------
  { "glActiveTexture", (uintptr_t)&glActiveTextureHook },
  { "glAttachShader", (uintptr_t)&glAttachShaderHook },
  { "glBindBuffer", (uintptr_t)&glBindBufferHook },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebufferHook },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbufferHook },
  { "glBindTexture", (uintptr_t)&glBindTextureHook },
  { "glBlendEquation", (uintptr_t)&glBlendEquationHook },
  { "glBlendFunc", (uintptr_t)&glBlendFuncHook },
  { "glBufferData", (uintptr_t)&glBufferDataHook },
  { "glClear", (uintptr_t)&glClearHook },
  { "glClearColor", (uintptr_t)&glClearColorHook },
  { "glClearDepthf", (uintptr_t)&glClearDepthfHook },
  { "glClearStencil", (uintptr_t)&glClearStencilHook },
  { "glColorMask", (uintptr_t)&glColorMaskHook },
  { "glCompileShader", (uintptr_t)&glCompileShaderHook },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2DHook },
  { "glCreateProgram", (uintptr_t)&glCreateProgramHook },
  { "glCreateShader", (uintptr_t)&glCreateShaderHook },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffersHook },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffersHook },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgramHook },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffersHook },
  { "glDeleteShader", (uintptr_t)&glDeleteShaderHook },
  { "glDeleteTextures", (uintptr_t)&glDeleteTexturesHook },
  { "glDepthFunc", (uintptr_t)&glDepthFuncHook },
  { "glDepthMask", (uintptr_t)&glDepthMaskHook },
  { "glDepthRangef", (uintptr_t)&glDepthRangefHook },
  { "glDisable", (uintptr_t)&glDisableHook },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArrayHook },
  { "glDrawArrays", (uintptr_t)&glDrawArraysHook },
  { "glDrawElements", (uintptr_t)&glDrawElementsHook },
  { "glEnable", (uintptr_t)&glEnableHook },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArrayHook },
  { "glFinish", (uintptr_t)&glFinishHook },
  { "glFlush", (uintptr_t)&glFlushHook },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbufferHook },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2DHook },
  { "glFrontFace", (uintptr_t)&glFrontFaceHook },
  { "glGenBuffers", (uintptr_t)&glGenBuffersHook },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffersHook },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffersHook },
  { "glGenTextures", (uintptr_t)&glGenTexturesHook },
  { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttribHook },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniformHook },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocationHook },
  { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameterivHook },
  { "glGetError", (uintptr_t)&glGetErrorHook },
  { "glGetIntegerv", (uintptr_t)&glGetIntegervHook },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLogHook },
  { "glGetProgramiv", (uintptr_t)&glGetProgramivHook },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },
  { "glGetShaderiv", (uintptr_t)&glGetShaderivHook },
  { "glGetString", (uintptr_t)&glGetStringHook },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocationHook },
  { "glLinkProgram", (uintptr_t)&glLinkProgramHook },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffsetHook },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorageHook },
  { "glScissor", (uintptr_t)&glScissorHook },
  { "glShaderSource", (uintptr_t)&glShaderSourceHook },
  { "glStencilFunc", (uintptr_t)&glStencilFuncHook },
  { "glStencilMask", (uintptr_t)&glStencilMaskHook },
  { "glStencilOp", (uintptr_t)&glStencilOpHook },
  { "glTexImage2D", (uintptr_t)&glTexImage2DHook },
  { "glTexParameteri", (uintptr_t)&glTexParameteriHook },
  { "glUniform1fv", (uintptr_t)&glUniform1fvHook },
  { "glUniform1i", (uintptr_t)&glUniform1iHook },
  { "glUniform2fv", (uintptr_t)&glUniform2fvHook },
  { "glUniform3fv", (uintptr_t)&glUniform3fvHook },
  { "glUniform4fv", (uintptr_t)&glUniform4fvHook },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fvHook },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fvHook },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fvHook },
  { "glUseProgram", (uintptr_t)&glUseProgramHook },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointerHook },
  { "glViewport", (uintptr_t)&glViewportHook },

  // --- OpenSL ES (vendored libOpenSLES) ------------------------------------
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
  { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "slCreateEngine", (uintptr_t)&slCreateEngineHook },

  // --- pthread (pthr.c soloader wrappers) ----------------------------------
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },

  // --- semaphores (libc_shim void**-indirection) ---------------------------
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- v2.2.1.06 additional imports ----------------------------------------
  // fortified (_chk) libc variants
  { "__memcpy_chk", (uintptr_t)&memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&memset_chk_fake },
  { "__strcpy_chk", (uintptr_t)&strcpy_chk_fake },
  { "__strncpy_chk", (uintptr_t)&strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&strncpy_chk2_fake },
  { "__strcat_chk", (uintptr_t)&strcat_chk_fake },
  { "__strlen_chk", (uintptr_t)&strlen_chk_fake },
  { "__strchr_chk", (uintptr_t)&strchr_chk_fake },
  { "__strrchr_chk", (uintptr_t)&strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&vsprintf_chk_fake },
  // libc / bionic
  { "calloc", (uintptr_t)&calloc },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "__android_log_print", (uintptr_t)&android_log_print_fake },
  // pthread: bionic-translating shims (pthr.c) where layouts differ; key/specific
  // use a 4-byte int key in both ABIs, so newlib's are binary-compatible.
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  // AAsset (asset manager open + fd-for-streamed-asset)
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  // no config-driven hook swaps
}

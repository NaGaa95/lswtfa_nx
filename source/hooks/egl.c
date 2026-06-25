/* egl.c -- wrapper-owned EGL + thin pass-throughs for TFA
 *
 * TFA's libProject_Douglas_HH.so is a classic Android GLSurfaceView client:
 * it does NOT import eglGetDisplay/eglInitialize/eglCreateWindowSurface/
 * eglSwapBuffers -- the Java GLSurfaceView owned all of that. Here the WRAPPER
 * owns it (egl_bringup / egl_present / egl_resize_surface) against the libnx
 * default NWindow + mesa-nouveau, and drives the GL natives (nativeInit /
 * nativeRender) on this thread while the context is current.
 *
 * The engine still creates its OWN second, SHARED context on its async
 * resource-loader thread ("cacheld") via eglCreateContext(share=window ctx) +
 * eglCreatePbufferSurface + eglMakeCurrent. We let those go straight to real
 * mesa: two genuine ES2 contexts sharing one display across two threads is the
 * configuration the engine was written for and that desktop GLES drivers
 * support. (If a given mesa build cannot honour cross-thread sharing, the
 * loader uploads would be invisible -- the localised fix is to alias the loader
 * context onto the window context here, the LSWTCS approach. Not needed on the
 * driver this was developed against.)
 *
 * Only glGetString is wrapped (the engine deref's vendor/renderer without a
 * NULL check); every other GL call binds straight to mesa (see imports.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"
#include "../fps.h"

// ---------------------------------------------------------------------------
// wrapper-owned window EGL state
// ---------------------------------------------------------------------------

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig  g_cfg = 0;

volatile int egl_swap_count = 0;
// system tick of the most recent shader compile/link; the monitor thread uses
// it to CPU-boost ONLY while shaders are actively compiling (boot + loading
// screens), and drop back to normal clocks otherwise.
volatile unsigned long long egl_last_compile_tick = 0;

// ---------------------------------------------------------------------------
// Single-context ownership handover.
//
// mesa-nouveau does NOT honour cross-context object sharing here: shaders and
// textures created on the engine's loader-thread context are invisible to the
// render-thread context (link errors "lacks a vertex shader", black textures).
// So we use exactly ONE real GL context (g_ctx) for BOTH threads, handed back
// and forth: a thread must "own" the context (have it really current) before it
// issues any GL. Ownership is sticky -- the owner keeps it across its calls for
// free -- and released cooperatively when another thread asks (release_req) at
// the owner's next GL-call boundary, or when the owner blocks (pthr/sleep call
// egl_gl_ownership_park). This both serialises GL (only the owner runs it) and
// keeps every object in one namespace (uploads are visible).
// ---------------------------------------------------------------------------

static Mutex   ho_mtx;
static CondVar ho_cond;
static volatile u64 ctx_owner = 0;   // tid that currently has g_ctx bound; 0 = none
static volatile int release_req = 0; // a non-owner is waiting for the context

static u64 cur_tid64(void) {
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);
  return tid;
}

// take ownership of the one real context on the calling thread (blocks until
// the previous owner releases). Called at the top of every GL hook.
static void gl_acquire(void) {
  const u64 me = cur_tid64();
  if (__atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) == me)
    return; // fast path: already ours, context stays current
  mutexLock(&ho_mtx);
  while (ctx_owner != 0 && ctx_owner != me) {
    release_req = 1;
    condvarWaitTimeout(&ho_cond, &ho_mtx, 5000000ull); // 5ms watchdog slices
  }
  if (ctx_owner != me) {
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
    __atomic_store_n(&ctx_owner, me, __ATOMIC_RELEASE);
  }
  release_req = 0;
  mutexUnlock(&ho_mtx);
}

// release the context if this thread owns it and someone is waiting. Used at
// SAFE boundaries only: the render thread's frame boundary (egl_present) and
// blocking points (egl_gl_ownership_park from pthr/sleep) -- NOT per GL call.
static void gl_release_if_wanted(void) {
  if (!__atomic_load_n(&release_req, __ATOMIC_ACQUIRE))
    return;
  const u64 me = cur_tid64();
  mutexLock(&ho_mtx);
  if (ctx_owner == me && release_req) {
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    __atomic_store_n(&ctx_owner, 0, __ATOMIC_RELEASE);
    condvarWakeAll(&ho_cond);
  }
  mutexUnlock(&ho_mtx);
}

// Per-GL-call bottom hook: deliberately a NO-OP. The owner keeps the context
// across its whole transaction (the render thread across a frame, the loader
// across a burst). Handing it over mid-call corrupts shared bind state (e.g.
// the element-buffer binding between glBindBuffer and glDrawElements -> null
// index -> crash). Ownership only changes at frame boundaries and blocking
// points, where no partial GL state is in flight.
static inline void gl_noop(void) {}

#define GLL() gl_acquire()
#define GLU() gl_noop()

EGLDisplay egl_display(void) { return g_dpy; }

static EGLConfig choose_config(EGLDisplay dpy) {
  // The engine treats the EGLConfig opaquely (it never queries attributes), so
  // we pick one that mesa-nouveau actually exposes. Try the preferred
  // RGBA8/d24/s8 window+pbuffer config, then relax.
  static const EGLint preferred[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_NONE
  };
  static const EGLint window_only[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  static const EGLint minimal[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  const EGLint *lists[] = { preferred, window_only, minimal };
  for (unsigned i = 0; i < 3; i++) {
    EGLConfig cfg = 0; EGLint n = 0;
    if (eglChooseConfig(dpy, lists[i], &cfg, 1, &n) == EGL_TRUE && n > 0) {
      debugPrintf("EGL: chose config %p via attrib list %u\n", cfg, i);
      return cfg;
    }
  }
  debugPrintf("EGL: no config matched!\n");
  return 0;
}

int egl_bringup(void) {
  mutexInit(&ho_mtx);
  condvarInit(&ho_cond);
  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_dpy == EGL_NO_DISPLAY) {
    debugPrintf("EGL: eglGetDisplay failed\n");
    return -1;
  }
  EGLint major = 0, minor = 0;
  if (eglInitialize(g_dpy, &major, &minor) != EGL_TRUE) {
    debugPrintf("EGL: eglInitialize failed (0x%x)\n", eglGetError());
    return -1;
  }
  debugPrintf("EGL: initialized v%d.%d\n", major, minor);
  eglBindAPI(EGL_OPENGL_ES_API);

  g_cfg = choose_config(g_dpy);
  if (!g_cfg)
    return -1;

  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, screen_width, screen_height);
  g_surf = eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)nw, NULL);
  if (g_surf == EGL_NO_SURFACE) {
    debugPrintf("EGL: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
    return -1;
  }

  const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attribs);
  if (g_ctx == EGL_NO_CONTEXT) {
    debugPrintf("EGL: eglCreateContext failed (0x%x)\n", eglGetError());
    return -1;
  }

  if (eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) != EGL_TRUE) {
    debugPrintf("EGL: eglMakeCurrent failed (0x%x)\n", eglGetError());
    return -1;
  }
  // this (render) thread now owns the one real context
  __atomic_store_n(&ctx_owner, cur_tid64(), __ATOMIC_RELEASE);
  eglSwapInterval(g_dpy, 2); // 30 fps: TFA is a fixed-timestep 30 fps game
  debugPrintf("EGL: window context current (%dx%d, dpy=%p surf=%p ctx=%p cfg=%p)\n",
              screen_width, screen_height, g_dpy, g_surf, g_ctx, g_cfg);
  return 0;
}

void egl_present(void) {
  gl_acquire(); // render thread owns the context for the whole frame up to here
  if (config.show_fps)
    fps_render();
  if (eglSwapBuffers(g_dpy, g_surf) != EGL_TRUE) {
    static int nfail = 0;
    if (nfail++ < 20)
      debugPrintf("EGL: eglSwapBuffers FAILED (0x%x)\n", eglGetError());
  }
  ++egl_swap_count;
  // frame boundary: safe point to hand the context to a waiting loader thread
  gl_release_if_wanted();
}

void egl_resize_surface(int w, int h) {
  if (g_dpy == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_surf != EGL_NO_SURFACE)
    eglDestroySurface(g_dpy, g_surf);
  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, w, h);
  g_surf = eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)nw, NULL);
  eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
  debugPrintf("EGL: surface resized to %dx%d (surf=%p)\n", w, h, g_surf);
}

// ---------------------------------------------------------------------------
// GL-ownership handover hooks, called from pthr.c / libc_shim.c when a thread
// is about to block (cond_wait, mutex contention, sleep) or is done with GL.
// A blocking owner MUST drop the context or the other thread can never get it.
// ---------------------------------------------------------------------------

// release the context if this thread holds it (used at blocking points + exit)
void egl_gl_ownership_park(void) {
  const u64 me = cur_tid64();
  if (__atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) != me)
    return;
  mutexLock(&ho_mtx);
  if (ctx_owner == me) {
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    __atomic_store_n(&ctx_owner, 0, __ATOMIC_RELEASE);
    release_req = 0;
    condvarWakeAll(&ho_cond);
  }
  mutexUnlock(&ho_mtx);
}
void egl_gl_ownership_release(void) { egl_gl_ownership_park(); }
void egl_gl_service_handover(void) { gl_release_if_wanted(); }
// take ownership of the one shared GL context on the calling thread (for code
// outside the GL hooks that must issue GL directly, e.g. the cutscene player).
void egl_gl_acquire(void) { gl_acquire(); }
int  egl_gl_thread_holds_context(void) {
  return __atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) == cur_tid64();
}

// ---------------------------------------------------------------------------
// fake pbuffer fallback: if mesa rejects a real pbuffer surface for the loader
// thread, hand out a token surface and bind the loader context surfaceless.
// (Real pbuffers normally succeed on switch-mesa; this is belt-and-braces.)
// ---------------------------------------------------------------------------

#define FAKE_PBUFFER_MAGIC 0x50425546u /* "PBUF" */
#define MAX_FAKE_PBUFFERS 8

typedef struct { uint32_t magic; EGLint width, height; } FakePbuffer;
static FakePbuffer *fake_pbuffers[MAX_FAKE_PBUFFERS];
static int fake_pbuffer_count = 0;

static FakePbuffer *fake_from_surface(EGLSurface s) {
  for (int i = 0; i < fake_pbuffer_count; i++)
    if ((EGLSurface)fake_pbuffers[i] == s) return fake_pbuffers[i];
  return NULL;
}

static EGLint attrib_value(const EGLint *attribs, EGLint key, EGLint fb) {
  if (!attribs) return fb;
  for (int i = 0; attribs[i] != EGL_NONE; i += 2)
    if (attribs[i] == key) return attribs[i + 1];
  return fb;
}

// ---------------------------------------------------------------------------
// pass-through hooks bound to the game's EGL imports
// ---------------------------------------------------------------------------

EGLBoolean eglBindAPIHook(EGLenum api) {
  GLL();
  EGLBoolean r = eglBindAPI(api);
  GLU();
  return r;
}

EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config) {
  GLL();
  EGLBoolean r = eglChooseConfig(dpy, attrib_list, configs, config_size, num_config);
  if (!(r == EGL_TRUE && num_config && *num_config > 0)) {
    // the engine's loader request matched nothing: relax to a known-good ES2 list
    static const EGLint relaxed[] = {
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE
    };
    r = eglChooseConfig(dpy, relaxed, configs, config_size, num_config);
    debugPrintf("EGL: eglChooseConfig relaxed -> %d (%d configs)\n",
                r, num_config ? *num_config : -1);
  }
  GLU();
  return r;
}

// Single-context model: the engine's loader context is FAKE. We never create a
// second real context (cross-context sharing is broken on mesa-nouveau); all GL
// from every thread runs on the one real g_ctx via the ownership handover. The
// engine just round-trips this token through eglMakeCurrent, which is virtual.
#define FAKE_CONTEXT_TOKEN ((EGLContext)0x0CC1F00D)

EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  debugPrintf("EGL: eglCreateContext(share=%p) -> fake token (one real ctx)\n", share_context);
  return FAKE_CONTEXT_TOKEN;
}

EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list) {
  (void)dpy; (void)config;
  // the loader never really renders to this; hand out a fake 1x1 token.
  if (fake_pbuffer_count >= MAX_FAKE_PBUFFERS)
    return (EGLSurface)fake_pbuffers[0];
  FakePbuffer *f = calloc(1, sizeof(*f));
  if (!f) return EGL_NO_SURFACE;
  f->magic = FAKE_PBUFFER_MAGIC;
  f->width = attrib_value(attrib_list, EGL_WIDTH, 1);
  f->height = attrib_value(attrib_list, EGL_HEIGHT, 1);
  fake_pbuffers[fake_pbuffer_count++] = f;
  debugPrintf("EGL: eglCreatePbufferSurface -> fake %p (%dx%d)\n",
              (void *)f, f->width, f->height);
  return (EGLSurface)f;
}

EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  // Virtual: the real binding of the one g_ctx is done lazily by gl_acquire()
  // on whichever thread next issues GL. The engine's loader thread calls this
  // to "activate" its (fake) context; we just accept it.
  (void)dpy; (void)draw; (void)read; (void)ctx;
  return EGL_TRUE;
}

EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  FakePbuffer *f = fake_from_surface(surface);
  if (f) {
    if (value) *value = (attribute == EGL_WIDTH) ? f->width
                       : (attribute == EGL_HEIGHT) ? f->height : 0;
    return EGL_TRUE;
  }
  GLL();
  EGLBoolean r = eglQuerySurface(dpy, surface, attribute, value);
  GLU();
  // mesa reports the window surface as 0x0 until the first buffer is acquired;
  // the engine trusts this and would build a 0x0 render target (black screen).
  if (r == EGL_TRUE && value && *value == 0) {
    if (attribute == EGL_WIDTH)  { *value = screen_width;  return r; }
    if (attribute == EGL_HEIGHT) { *value = screen_height; return r; }
  }
  return r;
}

EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval) {
  // TFA is a fixed-timestep 30 fps game (one sim step per presented frame).
  // Forcing interval 1 (60 fps) ran the simulation at DOUBLE speed, so honor the
  // engine's interval-2 request -> 30 fps. (main.c also caps the loop to 30 fps
  // in case the driver doesn't block the full two vsyncs.)
  (void)interval;
  GLL();
  EGLBoolean r = eglSwapInterval(dpy, 2);
  GLU();
  return r;
}

void *eglGetProcAddressHook(const char *name) {
  void *p = (void *)eglGetProcAddress(name);
  if (name && !strcmp(name, "glGetString"))
    p = (void *)glGetStringHook;
  return p;
}

// ---------------------------------------------------------------------------
// GL call hooks. Every GL entry point the engine imports is wrapped so it runs
// only while this thread owns the one real context (GLL/GLU = gl_acquire /
// gl_maybe_release). Shader compile/link are additionally logged.
// ---------------------------------------------------------------------------

#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

// shader/program pipeline. egl_last_compile_tick (bumped after compile/link)
// drives the monitor thread's CPU boost during loading screens.
void glCompileShaderHook(GLuint shader) {
  GLL();
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    static char buf[1024]; GLsizei n = 0;
    glGetShaderInfoLog(shader, sizeof(buf), &n, buf);
    debugPrintf("GL: shader %u COMPILE FAILED: %s\n", shader, buf);
  }
  egl_last_compile_tick = armGetSystemTick();
  GLU();
}
void glLinkProgramHook(GLuint program) {
  GLL();
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    static char buf[1024]; GLsizei n = 0;
    glGetProgramInfoLog(program, sizeof(buf), &n, buf);
    debugPrintf("GL: program %u LINK FAILED: %s\n", program, buf);
  }
  egl_last_compile_tick = armGetSystemTick();
  GLU();
}
GLuint glCreateShaderHook(GLenum type) { GLL(); GLuint r = glCreateShader(type); GLU(); return r; }
GLuint glCreateProgramHook(void) { GLL(); GLuint r = glCreateProgram(); GLU(); return r; }
void glShaderSourceHook(GLuint s, GLsizei c, const GLchar *const *str, const GLint *len) { GLL(); glShaderSource(s, c, str, len); GLU(); }
void glAttachShaderHook(GLuint p, GLuint s) { GLL(); glAttachShader(p, s); GLU(); }
void glBindAttribLocationHook(GLuint p, GLuint i, const GLchar *n) { GLL(); glBindAttribLocation(p, i, n); GLU(); }
void glUseProgramHook(GLuint p) { GLL(); glUseProgram(p); GLU(); }
void glDeleteShaderHook(GLuint s) { GLL(); glDeleteShader(s); GLU(); }
void glDeleteProgramHook(GLuint p) { GLL(); glDeleteProgram(p); GLU(); }
void glGetActiveAttribHook(GLuint p, GLuint i, GLsizei b, GLsizei *l, GLint *sz, GLenum *t, GLchar *n) { GLL(); glGetActiveAttrib(p, i, b, l, sz, t, n); GLU(); }
void glGetActiveUniformHook(GLuint p, GLuint i, GLsizei b, GLsizei *l, GLint *sz, GLenum *t, GLchar *n) { GLL(); glGetActiveUniform(p, i, b, l, sz, t, n); GLU(); }
GLint glGetAttribLocationHook(GLuint p, const GLchar *n) { GLL(); GLint r = glGetAttribLocation(p, n); GLU(); return r; }
GLint glGetUniformLocationHook(GLuint p, const GLchar *n) { GLL(); GLint r = glGetUniformLocation(p, n); GLU(); return r; }
void glGetProgramInfoLogHook(GLuint p, GLsizei b, GLsizei *l, GLchar *log) { GLL(); glGetProgramInfoLog(p, b, l, log); GLU(); }
void glGetProgramivHook(GLuint p, GLenum n, GLint *v) { GLL(); glGetProgramiv(p, n, v); GLU(); }
void glGetShaderInfoLogHook(GLuint s, GLsizei b, GLsizei *l, GLchar *log) { GLL(); glGetShaderInfoLog(s, b, l, log); GLU(); }
void glGetShaderivHook(GLuint s, GLenum n, GLint *v) { GLL(); glGetShaderiv(s, n, v); GLU(); }

// uniforms
void glUniform1fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform1fv(l, c, v); GLU(); }
void glUniform1iHook(GLint l, GLint v0) { GLL(); glUniform1i(l, v0); GLU(); }
void glUniform2fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform2fv(l, c, v); GLU(); }
void glUniform3fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform3fv(l, c, v); GLU(); }
void glUniform4fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform4fv(l, c, v); GLU(); }
void glUniformMatrix2fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix2fv(l, c, t, v); GLU(); }
void glUniformMatrix3fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix3fv(l, c, t, v); GLU(); }
void glUniformMatrix4fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix4fv(l, c, t, v); GLU(); }

// vertex attribs
void glVertexAttribPointerHook(GLuint i, GLint s, GLenum t, GLboolean nm, GLsizei st, const void *p) { GLL(); glVertexAttribPointer(i, s, t, nm, st, p); GLU(); }
void glEnableVertexAttribArrayHook(GLuint i) { GLL(); glEnableVertexAttribArray(i); GLU(); }
void glDisableVertexAttribArrayHook(GLuint i) { GLL(); glDisableVertexAttribArray(i); GLU(); }

// textures / buffers / renderbuffers / framebuffers (object create/upload)
void glActiveTextureHook(GLenum t) { GLL(); glActiveTexture(t); GLU(); }
void glBindTextureHook(GLenum t, GLuint tex) { GLL(); glBindTexture(t, tex); GLU(); }
void glGenTexturesHook(GLsizei n, GLuint *t) { GLL(); glGenTextures(n, t); GLU(); }
void glDeleteTexturesHook(GLsizei n, const GLuint *t) { GLL(); glDeleteTextures(n, t); GLU(); }
void glTexImage2DHook(GLenum tg, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void *px) { GLL(); glTexImage2D(tg, lv, ifmt, w, h, b, fmt, ty, px); GLU(); }
void glCompressedTexImage2DHook(GLenum tg, GLint lv, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d) { GLL(); glCompressedTexImage2D(tg, lv, ifmt, w, h, b, sz, d); GLU(); }
void glTexParameteriHook(GLenum tg, GLenum p, GLint v) { GLL(); glTexParameteri(tg, p, v); GLU(); }
void glBindBufferHook(GLenum t, GLuint b) { GLL(); glBindBuffer(t, b); GLU(); }
void glGenBuffersHook(GLsizei n, GLuint *b) { GLL(); glGenBuffers(n, b); GLU(); }
void glDeleteBuffersHook(GLsizei n, const GLuint *b) { GLL(); glDeleteBuffers(n, b); GLU(); }
void glBufferDataHook(GLenum t, GLsizeiptr sz, const void *d, GLenum u) { GLL(); glBufferData(t, sz, d, u); GLU(); }
void glGetBufferParameterivHook(GLenum t, GLenum p, GLint *v) { GLL(); glGetBufferParameteriv(t, p, v); GLU(); }
void glBindFramebufferHook(GLenum t, GLuint f) { GLL(); glBindFramebuffer(t, f); GLU(); }
void glGenFramebuffersHook(GLsizei n, GLuint *f) { GLL(); glGenFramebuffers(n, f); GLU(); }
void glDeleteFramebuffersHook(GLsizei n, const GLuint *f) { GLL(); glDeleteFramebuffers(n, f); GLU(); }
void glFramebufferTexture2DHook(GLenum tg, GLenum at, GLenum tt, GLuint tex, GLint lv) { GLL(); glFramebufferTexture2D(tg, at, tt, tex, lv); GLU(); }
void glFramebufferRenderbufferHook(GLenum tg, GLenum at, GLenum rt, GLuint rb) { GLL(); glFramebufferRenderbuffer(tg, at, rt, rb); GLU(); }
void glBindRenderbufferHook(GLenum t, GLuint rb) { GLL(); glBindRenderbuffer(t, rb); GLU(); }
void glGenRenderbuffersHook(GLsizei n, GLuint *rb) { GLL(); glGenRenderbuffers(n, rb); GLU(); }
void glDeleteRenderbuffersHook(GLsizei n, const GLuint *rb) { GLL(); glDeleteRenderbuffers(n, rb); GLU(); }
void glRenderbufferStorageHook(GLenum t, GLenum ifmt, GLsizei w, GLsizei h) { GLL(); glRenderbufferStorage(t, ifmt, w, h); GLU(); }

// draws + state
void glClearHook(GLbitfield m) { GLL(); glClear(m); GLU(); }
void glClearColorHook(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { GLL(); glClearColor(r, g, b, a); GLU(); }
void glClearDepthfHook(GLfloat d) { GLL(); glClearDepthf(d); GLU(); }
void glClearStencilHook(GLint s) { GLL(); glClearStencil(s); GLU(); }
void glDrawArraysHook(GLenum m, GLint f, GLsizei c) { GLL(); glDrawArrays(m, f, c); GLU(); }
void glDrawElementsHook(GLenum m, GLsizei c, GLenum t, const void *i) { GLL(); glDrawElements(m, c, t, i); GLU(); }
void glViewportHook(GLint x, GLint y, GLsizei w, GLsizei h) {
  GLL();
  glViewport(x, y, w, h);
  GLU();
}
void glScissorHook(GLint x, GLint y, GLsizei w, GLsizei h) { GLL(); glScissor(x, y, w, h); GLU(); }
void glEnableHook(GLenum c) { GLL(); glEnable(c); GLU(); }
void glDisableHook(GLenum c) { GLL(); glDisable(c); GLU(); }
void glBlendEquationHook(GLenum m) { GLL(); glBlendEquation(m); GLU(); }
void glBlendFuncHook(GLenum s, GLenum d) { GLL(); glBlendFunc(s, d); GLU(); }
void glDepthFuncHook(GLenum f) { GLL(); glDepthFunc(f); GLU(); }
void glDepthMaskHook(GLboolean f) { GLL(); glDepthMask(f); GLU(); }
void glDepthRangefHook(GLfloat n, GLfloat f) { GLL(); glDepthRangef(n, f); GLU(); }
void glColorMaskHook(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GLL(); glColorMask(r, g, b, a); GLU(); }
void glCullFaceHook(GLenum m) { GLL(); glCullFace(m); GLU(); }
void glFrontFaceHook(GLenum m) { GLL(); glFrontFace(m); GLU(); }
void glPolygonOffsetHook(GLfloat f, GLfloat u) { GLL(); glPolygonOffset(f, u); GLU(); }
void glStencilFuncHook(GLenum f, GLint r, GLuint m) { GLL(); glStencilFunc(f, r, m); GLU(); }
void glStencilMaskHook(GLuint m) { GLL(); glStencilMask(m); GLU(); }
void glStencilOpHook(GLenum f, GLenum zf, GLenum zp) { GLL(); glStencilOp(f, zf, zp); GLU(); }
void glFinishHook(void) { GLL(); glFinish(); GLU(); }
void glFlushHook(void) { GLL(); glFlush(); GLU(); }
void glGetIntegervHook(GLenum n, GLint *d) { GLL(); glGetIntegerv(n, d); GLU(); }
GLenum glGetErrorHook(void) { GLL(); GLenum r = glGetError(); GLU(); return r; }

const GLubyte *glGetStringHook(GLenum name) {
  GLL();
  const GLubyte *s = glGetString(name);
  GLU();
  if (s) {
    if (name == GL_VENDOR || name == GL_RENDERER || name == GL_VERSION)
      debugPrintf("GL: glGetString(0x%x) = %s\n", name, (const char *)s);
    return s;
  }
  switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"NVIDIA";
    case GL_RENDERER: return (const GLubyte *)"NVIDIA Tegra";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 2.0";
    case 0x8B8C:      return (const GLubyte *)"OpenGL ES GLSL ES 1.00"; // GL_SHADING_LANGUAGE_VERSION
    case GL_EXTENSIONS:
      return (const GLubyte *)"GL_OES_compressed_ETC1_RGB8_texture "
                              "GL_OES_depth24 GL_OES_packed_depth_stencil "
                              "GL_OES_depth_texture GL_EXT_texture_compression_dxt1";
    default:          return (const GLubyte *)"";
  }
}

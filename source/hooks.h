/* hooks.h -- TFA (libProject_Douglas_HH.so) hook surface
 *
 * TFA uses the classic Android GLSurfaceView model: the wrapper owns the EGL
 * display, window surface, context and presentation; the engine only queries
 * the current context and creates a SHARED loader context on its own thread.
 * So unlike the LSWTCS port, egl.c is a thin pass-through over native mesa --
 * the heavy single-context ownership-handover machinery is gone (its public
 * entry points survive as no-op stubs so pthr.c / libc_shim.c compile
 * unchanged). The game's GL calls bind straight to mesa (see imports.c); only
 * glGetString is wrapped (the engine deref's the result without NULL checks).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <EGL/egl.h>
#include <GLES2/gl2.h>

// engine-level patches applied to the loaded libProject_Douglas_HH.so
void patch_game(void);

// --- wrapper-owned EGL (egl.c) ---------------------------------------------
// Bring up display + window surface + ES2 context against the default NWindow
// and make it current on the calling (render) thread. Returns 0 on success.
int  egl_bringup(void);
// Present the current frame (optionally draws the FPS counter first).
void egl_present(void);
// Recreate the window surface at a new size (dock/undock) and re-make-current.
void egl_resize_surface(int w, int h);
// The display the wrapper brought up.
EGLDisplay egl_display(void);

// --- GL-ownership stubs (no-ops here; kept for pthr.c / libc_shim.c) --------
void egl_gl_ownership_park(void);
void egl_gl_ownership_release(void);
void egl_gl_service_handover(void);
void egl_gl_acquire(void);
int  egl_gl_thread_holds_context(void);

// presented-frame counter, read by main.c (boot detection / heartbeat)
extern volatile int egl_swap_count;
// system tick of the last shader compile/link; main.c boosts the CPU while this
// is recent (boot + loading screens), normal clocks otherwise.
extern volatile unsigned long long egl_last_compile_tick;

// --- EGL hooks bound to the game's imports (egl.c) --------------------------
EGLBoolean eglBindAPIHook(EGLenum api);
EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list);
EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list);
EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval);
void      *eglGetProcAddressHook(const char *name);

// glGetString: never hand the engine a NULL (it deref's vendor/renderer).
const GLubyte *glGetStringHook(GLenum name);

// --- GL serialization hooks -------------------------------------------------
// Every GL call the engine makes is wrapped so the render thread and the
// engine's loader thread never enter mesa-nouveau concurrently (it is not
// thread-safe across the shared context). See hooks/egl.c.
void   glCompileShaderHook(GLuint shader);
void   glLinkProgramHook(GLuint program);
GLuint glCreateShaderHook(GLenum type);
GLuint glCreateProgramHook(void);
void   glShaderSourceHook(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void   glAttachShaderHook(GLuint program, GLuint shader);
void   glBindAttribLocationHook(GLuint program, GLuint index, const GLchar *name);
void   glUseProgramHook(GLuint program);
void   glDeleteShaderHook(GLuint shader);
void   glDeleteProgramHook(GLuint program);
void   glGetActiveAttribHook(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void   glGetActiveUniformHook(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
GLint  glGetAttribLocationHook(GLuint program, const GLchar *name);
GLint  glGetUniformLocationHook(GLuint program, const GLchar *name);
void   glGetProgramInfoLogHook(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetProgramivHook(GLuint program, GLenum pname, GLint *params);
void   glGetShaderInfoLogHook(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetShaderivHook(GLuint shader, GLenum pname, GLint *params);
void   glUniform1fvHook(GLint location, GLsizei count, const GLfloat *value);
void   glUniform1iHook(GLint location, GLint v0);
void   glUniform2fvHook(GLint location, GLsizei count, const GLfloat *value);
void   glUniform3fvHook(GLint location, GLsizei count, const GLfloat *value);
void   glUniform4fvHook(GLint location, GLsizei count, const GLfloat *value);
void   glUniformMatrix2fvHook(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void   glUniformMatrix3fvHook(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void   glUniformMatrix4fvHook(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void   glVertexAttribPointerHook(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
void   glEnableVertexAttribArrayHook(GLuint index);
void   glDisableVertexAttribArrayHook(GLuint index);
void   glActiveTextureHook(GLenum texture);
void   glBindTextureHook(GLenum target, GLuint texture);
void   glGenTexturesHook(GLsizei n, GLuint *textures);
void   glDeleteTexturesHook(GLsizei n, const GLuint *textures);
void   glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void   glCompressedTexImage2DHook(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
void   glTexParameteriHook(GLenum target, GLenum pname, GLint param);
void   glBindBufferHook(GLenum target, GLuint buffer);
void   glGenBuffersHook(GLsizei n, GLuint *buffers);
void   glDeleteBuffersHook(GLsizei n, const GLuint *buffers);
void   glBufferDataHook(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void   glGetBufferParameterivHook(GLenum target, GLenum pname, GLint *params);
void   glBindFramebufferHook(GLenum target, GLuint framebuffer);
void   glGenFramebuffersHook(GLsizei n, GLuint *framebuffers);
void   glDeleteFramebuffersHook(GLsizei n, const GLuint *framebuffers);
void   glFramebufferTexture2DHook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void   glFramebufferRenderbufferHook(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void   glBindRenderbufferHook(GLenum target, GLuint renderbuffer);
void   glGenRenderbuffersHook(GLsizei n, GLuint *renderbuffers);
void   glDeleteRenderbuffersHook(GLsizei n, const GLuint *renderbuffers);
void   glRenderbufferStorageHook(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void   glClearHook(GLbitfield mask);
void   glClearColorHook(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void   glClearDepthfHook(GLfloat d);
void   glClearStencilHook(GLint s);
void   glDrawArraysHook(GLenum mode, GLint first, GLsizei count);
void   glDrawElementsHook(GLenum mode, GLsizei count, GLenum type, const void *indices);
void   glViewportHook(GLint x, GLint y, GLsizei width, GLsizei height);
void   glScissorHook(GLint x, GLint y, GLsizei width, GLsizei height);
void   glEnableHook(GLenum cap);
void   glDisableHook(GLenum cap);
void   glBlendEquationHook(GLenum mode);
void   glBlendFuncHook(GLenum sfactor, GLenum dfactor);
void   glDepthFuncHook(GLenum func);
void   glDepthMaskHook(GLboolean flag);
void   glDepthRangefHook(GLfloat n, GLfloat f);
void   glColorMaskHook(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void   glCullFaceHook(GLenum mode);
void   glFrontFaceHook(GLenum mode);
void   glPolygonOffsetHook(GLfloat factor, GLfloat units);
void   glStencilFuncHook(GLenum func, GLint ref, GLuint mask);
void   glStencilMaskHook(GLuint mask);
void   glStencilOpHook(GLenum fail, GLenum zfail, GLenum zpass);
void   glFinishHook(void);
void   glFlushHook(void);
void   glGetIntegervHook(GLenum pname, GLint *data);
GLenum glGetErrorHook(void);

#endif

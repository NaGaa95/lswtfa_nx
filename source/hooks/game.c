/* game.c -- binary patches/hooks on libProject_Douglas_HH.so (v2.2.1.06).
 *
 * All offsets here are version-specific (22106). Three patches:
 *  - the shader-preload busy-wait that deadlocks our single GL context,
 *  - the 0.75 render-scale constant (full-res scene),
 *  - the fnaFMV_* cutscene entry points (redirected to our ffmpeg player).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <switch.h>

#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../fmv.h"

extern so_module game_mod; // defined in main.c

// Cutscene player: the engine decodes .mp4s via Java (MediaPlayer/SurfaceTexture),
// which doesn't exist here, so we redirect fnaFMV_Open/Finished/Close to our own
// ffmpeg player (fmv.c). fnaFMV_Open plays the whole movie synchronously, then
// fnaFMV_Finished reports done; the other fnaFMV_* are ret-stubs (too small to
// hook). These three bodies are all >=16 bytes (hookable).
#define OFF_FMV_OPEN     0x6009a8u
#define OFF_FMV_FINISHED 0x600db0u
#define OFF_FMV_CLOSE    0x600dc4u

// Shader-preload busy-wait: GameLoopModule::LoadPostWorldLoad ends a level load
// with `while (!UILoading::PreLoadShadersDone()) ;` -- a tight spin that makes no
// GL call and never yields, so on our single shared GL context it deadlocks (the
// spinner holds the context the render thread needs to set the flag). We redirect
// PreLoadShadersDone's PLT stub to a shim that hands the context back + yields,
// then returns the real flag.
#define OFF_PRELOAD_DONE_PLT 0x2d0920u // UILoading::PreLoadShadersDone()@plt
#define OFF_PRELOAD_DONE_FN  0x4e8314u // UILoading::PreLoadShadersDone() body

typedef unsigned char (*preload_done_fn)(void);
static preload_done_fn g_real_preload_done = NULL;

static unsigned char PreLoadShadersDone_shim(void) {
  egl_gl_service_handover();  // release the GL context to a waiting thread (render)
  svcSleepThread(500000);     // 0.5ms: don't peg the core during the engine's busy-wait
  return g_real_preload_done ? g_real_preload_done() : 1;
}

void patch_game(void) {
  // resolve the real getter at its runtime (RX) address; patch the PLT stub via
  // the writable load_base mirror (this runs before so_finalize maps it RX).
  g_real_preload_done =
      (preload_done_fn)((uintptr_t)game_mod.load_virtbase + OFF_PRELOAD_DONE_FN);
  hook_arm64((uintptr_t)game_mod.load_base + OFF_PRELOAD_DONE_PLT,
             (uintptr_t)&PreLoadShadersDone_shim);
  debugPrintf("patch_game: hooked PreLoadShadersDone busy-wait (plt 0x%x -> shim)\n",
              OFF_PRELOAD_DONE_PLT);

  // --- full-resolution render -------------------------------------------------
  // fnaRender_Init sizes the scene colour buffer as screen_size * 0.75, so the
  // 3D scene renders at 960x540 and is upscaled to 720p (soft/low-res). Patch
  // the scale constant (fmov v0.2s,#0.75 @0x610440) to 1.0 -> scene renders at
  // the full screen resolution.
  uint32_t *scale_insn = (uint32_t *)((uintptr_t)game_mod.load_base + 0x610440u);
  if (*scale_insn == 0x0f03f500u) {        // fmov v0.2s, #0.75
    *scale_insn = 0x0f03f600u;             // fmov v0.2s, #1.0
    debugPrintf("patch_game: render scale 0.75 -> 1.0 (full-res scene)\n");
  } else {
    debugPrintf("patch_game: WARN render-scale insn was 0x%08x (expected 0x0f03f500), skipped\n",
                *scale_insn);
  }

  // cutscene player: redirect the engine's fnaFMV_* bodies to our ffmpeg-based
  // implementation (see fmv.c). Patched via the writable load_base mirror.
  hook_arm64((uintptr_t)game_mod.load_base + OFF_FMV_OPEN,     (uintptr_t)&fmv_hook_open);
  hook_arm64((uintptr_t)game_mod.load_base + OFF_FMV_FINISHED, (uintptr_t)&fmv_hook_finished);
  hook_arm64((uintptr_t)game_mod.load_base + OFF_FMV_CLOSE,    (uintptr_t)&fmv_hook_close);
  debugPrintf("patch_game: hooked fnaFMV_Open/Finished/Close (ffmpeg cutscene player)\n");
}

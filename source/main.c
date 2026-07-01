/* main.c
 *
 * LEGO Star Wars: The Force Awakens (Android, arm64) on the Switch.
 *
 * libProject_Douglas_HH.so is the WB Games "Fusion" engine, driven through the
 * classic Android GLSurfaceView contract: the Java side (here: the wrapper)
 * owns the EGL context and calls nativeRender() once per frame on the GL
 * thread. We reproduce GameActivity.onCreate + the GLSurfaceView render thread:
 *
 *   boot thread: load/relocate/resolve/init the .so, set the save/cache/write
 *     paths + device strings + audio buffer, mount the OBB into the engine VFS.
 *   render thread (= this thread): bring up EGL ourselves, nativeInit /
 *     nativeResize / nativeResume, then loop { pump input; nativeRender; swap }.
 *
 * Bootstrap order, signatures and addresses were recovered from classes.dex +
 * the .so disassembly; see _re/specs/{A-bootstrap,C-egl-render,D-...}.md.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "pthr.h"
#include "fmv.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod; // libProject_Douglas_HH.so

// Fake JNI handles passed to the natives. jni_fake.c never dereferences the
// jobject for these (GetObjectClass / GetIntField / AAssetManager_fromJava /
// CallObjectMethod all ignore the object value), so sentinels are sufficient;
// only the string arguments are real jni_make_string objects.
#define FUSION_OBJ    ((void *)0x46555331) // "FUS1"
#define GLSV_OBJ      ((void *)0x474c5631) // "GLV1"
#define ACTIVITY_OBJ  ((void *)0x41435431) // "ACT1"
#define EGLCONFIG_OBJ ((void *)0x45474331) // "EGC1"
#define ASSETMGR_OBJ  ((void *)0x41534d31) // "ASM1"

// separate the newlib heap from the .so load region
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // The newlib heap backs BOTH the game's malloc and mesa's host-side
  // allocations (the nouveau GLSL compiler is memory hungry). Reserve a fixed
  // slice for the loaded .so image + headroom and give ALL the rest to newlib.
  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  // SO_NAME = the v2.2.1.06 engine; the .fib is a representative extracted asset
  // (the user unpacks every asset pack's .../assets/ into GAMEDATA_DIR).
  const char *files[] = { SO_NAME, GAMEDATA_DIR "/project_douglas_mobile.fib" };
  struct stat st;
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\n\nCopy the v2.2.1.06 %s and extract\nthe asset packs into /switch/lswtfa/%s/.",
                  files[i], SO_NAME, GAMEDATA_DIR);
  }
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// resolved entry points (libProject_Douglas_HH.so)
// ---------------------------------------------------------------------------

static struct {
  // Fusion (engine config / VFS / input) -- run on the boot/UI thread
  void (*setWritePath)(void *env, void *obj, void *jstr);
  void (*setSavePath)(void *env, void *obj, void *jstr);
  void (*setCachePath)(void *env, void *obj, void *jstr);
  void (*setDeviceStrings)(void *env, void *obj, void *m, void *p, void *mf, void *hw);
  void (*setAudioOutputBufferSize)(void *env, void *obj, int frames);
  void (*initAssetManager)(void *env, void *obj, void *mgr);
  void (*controllerSetData)(void *env, void *obj, int dev, int mask, float x, float y);
  int  (*backButtonPressed)(void *env, void *obj);
  void (*touchDown)(void *env, void *obj, int id, float x, float y, float p);
  void (*touchMove)(void *env, void *obj, int id, float x, float y, float p);
  void (*touchUp)(void *env, void *obj, int id, float x, float y, float p);

  // GameGLSurfaceView (GL natives) -- run on the EGL-owning render thread
  void (*nativeInit)(void *env, void *obj, void *eglcfg, void *activity);
  void (*nativeResize)(void *env, void *obj, int w, int h);
  void (*nativeRender)(void *env, void *obj);
  void (*nativeResume)(void *env, void *obj);
  void (*nativePause)(void *env, void *obj);
  void (*nativeWindowFocusChanged)(void *env, void *obj, int focused);
  void (*nativeColdBoot)(void *env, void *obj);
  void (*nativeDone)(void *env, void *obj);

  // engine asset-dir registration (resolved BEFORE so_finalize; the symbol table
  // lives in load_base, which finalize unmaps -- no lookups are valid afterwards)
  void (*addAssetDir)(const char *dir);
} g;

#define RESOLVE(field, sym) \
  g.field = (void *)so_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,              "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(setAudioOutputBufferSize, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAudioOutputBufferSize");
  RESOLVE(initAssetManager,         "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  RESOLVE(controllerSetData,        "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  RESOLVE(backButtonPressed,        "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
  RESOLVE(touchDown,                "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
  RESOLVE(touchMove,                "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventMove");
  RESOLVE(touchUp,                  "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");

  RESOLVE(nativeInit,               "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
  RESOLVE(nativeResize,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
  RESOLVE(nativeRender,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
  RESOLVE(nativeResume,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
  RESOLVE(nativePause,              "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
  RESOLVE(nativeWindowFocusChanged, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
  g.nativeColdBoot = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeColdBoot");
  g.nativeDone     = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeDone");

  // engine asset-dir registration (Fusion.addAssetsDirs -> fnOBBPackages_AddAssetDir);
  // resolve here, before so_finalize unmaps the symbol table
  g.addAssetDir = (void *)so_find_addr_rx(&game_mod, "_Z25fnOBBPackages_AddAssetDirPKc");
}

// ---------------------------------------------------------------------------
// boot sequence (mirrors GameActivity.onCreate, in order)
// ---------------------------------------------------------------------------

static void run_boot_sequence(void) {
  // 1. paths + device identity (no GL) -- ORDER MATTERS (verified call sites)
  g.setWritePath(fake_env, FUSION_OBJ, jni_make_string(WRITE_PATH));
  g.setSavePath (fake_env, FUSION_OBJ, jni_make_string(SAVE_PATH));
  g.setCachePath(fake_env, FUSION_OBJ, jni_make_string(CACHE_PATH));
  g.setDeviceStrings(fake_env, FUSION_OBJ,
                     jni_make_string(DEVICE_MODEL), jni_make_string(DEVICE_PRODUCT),
                     jni_make_string(DEVICE_MANUFACTURER), jni_make_string(DEVICE_HARDWARE));
  g.setAudioOutputBufferSize(fake_env, FUSION_OBJ, AUDIO_BUF_FRAMES);

  // 2. asset manager: lets the engine read loose APK assets/ (fonts, shaders)
  //    from ./assets via the libc_shim AAsset emulation; tolerated if absent.
  g.initAssetManager(fake_env, FUSION_OBJ, ASSETMGR_OBJ);

  // 3. register the loose game-data directory with the engine. The user extracts
  //    every asset pack's .../assets/ contents into GAMEDATA_DIR (relative to the
  //    game dir); the engine reads its .fib data, cutscenes/ and music/ from
  //    there. (v2.2.1.06 uses Play Asset Delivery loose files, not an OBB.)
  g.addAssetDir(GAMEDATA_DIR);
  debugPrintf("boot: registered asset dir '%s'\n", GAMEDATA_DIR);
}

// ---------------------------------------------------------------------------
// input pump (render thread, before each frame)
// ---------------------------------------------------------------------------

// Fusion button bitmask. ProcessJoypadController maps these four bits to the
// positional Controls_Pad* inputs; game actions are assigned from there.
enum {
  TFA_L2    = 0x0001, TFA_R2     = 0x0002,
  TFA_L1    = 0x0004, TFA_R1     = 0x0008,
  TFA_SOUTH = 0x0010, TFA_EAST   = 0x0020,
  TFA_WEST  = 0x0040, TFA_NORTH  = 0x0080,
  TFA_L3    = 0x0200, TFA_R3     = 0x0400,
  TFA_START = 0x0800,
};

static PadState pad;
static u64 pad_prev = 0;

// Switch sticks are circle-clamped (a full diagonal reads ~0.71 per axis) but
// the engine expects square axes; remap radially so diagonals reach full speed.
static void stick_circle_to_square(float *x, float *y) {
  const float ax = fabsf(*x), ay = fabsf(*y);
  const float m = (ax > ay) ? ax : ay;
  if (m < 1e-6f)
    return;
  const float s = sqrtf(*x * *x + *y * *y) / m;
  *x *= s; *y *= s;
  if (*x > 1.0f) *x = 1.0f; else if (*x < -1.0f) *x = -1.0f;
  if (*y > 1.0f) *y = 1.0f; else if (*y < -1.0f) *y = -1.0f;
}

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);

  int mask = 0;
  // Keep physical positions consistent between the engine and Switch:
  // B=south (jump), A=east, Y=west (attack), X=north (character switch).
  if (down & HidNpadButton_B)      mask |= TFA_SOUTH;
  if (down & HidNpadButton_A)      mask |= TFA_EAST;
  if (down & HidNpadButton_Y)      mask |= TFA_WEST;
  if (down & HidNpadButton_X)      mask |= TFA_NORTH;
  if (down & HidNpadButton_L)      mask |= TFA_L1;
  if (down & HidNpadButton_R)      mask |= TFA_R1;
  if (down & HidNpadButton_ZL)     mask |= TFA_L2;
  if (down & HidNpadButton_ZR)     mask |= TFA_R2;
  if (down & HidNpadButton_StickL) mask |= TFA_L3;
  if (down & HidNpadButton_StickR) mask |= TFA_R3;
  if (down & HidNpadButton_Plus)   mask |= TFA_START;

  // left stick: Android axes are right-positive / DOWN-positive; the engine
  // negates the Y we pass. Switch sticks are right-positive / up-positive, so
  // feed x as-is and y negated (-> Android down-positive).
  const float scale = 1.f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  float lx = (float)ls.x * scale;
  float ly = (float)ls.y * -scale;
  stick_circle_to_square(&lx, &ly);

  // d-pad drives movement too (LEGO games map it to the stick)
  if (down & HidNpadButton_Left)  lx = -1.0f;
  if (down & HidNpadButton_Right) lx =  1.0f;
  if (down & HidNpadButton_Up)    ly = -1.0f;
  if (down & HidNpadButton_Down)  ly =  1.0f;

  // NOTE: right stick intentionally unmapped for now. TFA's native input has
  // only one analog pair (this left stick) + the button mask, and the shoulder
  // buttons are NOT the camera (mapping the right stick to them triggered
  // character actions). Correct camera mapping pending (see notes).

  // deviceId 0 keeps the engine's "controller connected" gate set (!= -1)
  g.controllerSetData(fake_env, FUSION_OBJ, 0, mask, lx, ly);

  // Minus -> back/menu (rising edge)
  const u64 changed = down ^ pad_prev;
  if ((changed & HidNpadButton_Minus) && (down & HidNpadButton_Minus))
    g.backButtonPressed(fake_env, FUSION_OBJ);
  pad_prev = down;
}

#define MAX_TOUCHES 8
typedef struct { int active; u32 finger_id; float x, y; } TouchSlot;
static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == id) return i;
  return -1;
}
static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active) return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;

  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;

  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0) continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
      g.touchDown(fake_env, FUSION_OBJ, slot, x, y, 1.0f);
    } else if (x != touch_prev[slot].x || y != touch_prev[slot].y) {
      g.touchMove(fake_env, FUSION_OBJ, slot, x, y, 1.0f);
    }
    touch_prev[slot].x = x;
    touch_prev[slot].y = y;
    seen[slot] = 1;
  }

  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      g.touchUp(fake_env, FUSION_OBJ, slot, touch_prev[slot].x, touch_prev[slot].y, 0.0f);
      touch_prev[slot].active = 0;
    }
  }
}

// CPU-boost monitor: high clocks ONLY while shaders are actively compiling
// (boot bring-up + level loading screens), normal clocks the rest of the time.
// Boot/loads are where the nouveau GLSL compiler pegs the CPU; gameplay does
// not need the boost. Driven by egl_last_compile_tick (bumped in the compile/
// link hooks), so it follows real compilation, not frame timing.
static void monitor_fn(void *arg) {
  (void)arg;
  const u64 freq = armGetSystemTickFreq();
  const u64 window = (freq * 3) / 4; // "loading" if a shader compiled <0.75s ago
  int boosting = 1;                  // main() starts boosted for boot
  for (;;) {
    svcSleepThread(200000000ull); // 200 ms
    const u64 last = egl_last_compile_tick;
    const u64 now = armGetSystemTick();
    const int loading = (last != 0) && (now - last) < window;
    if (loading != boosting) {
      cpu_boost(loading);
      boosting = loading;
    }
  }
}
static Thread g_monitor;

int main(int argc, char *argv[]) {
  // make the .nro's own folder the cwd, so the OBB, config and saves are found
  // relative to it regardless of how the launcher set the working directory.
  if (argc > 0 && argv[0]) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = '\0';
      chdir(dir);
    }
  }

  cpu_boost(1);

  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));
  debugPrintf("heap: newlib %u MB, lib base %p, lib region %u MB\n",
              heap_mb, heap_so_base, (unsigned)(heap_so_limit / (1024 * 1024)));

  // launched as an applet (album/hbmenu without a title override) we only get
  // ~0.5 GB -- the game needs far more and would die in a confusing OOM crash.
  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "this port from there.", heap_mb);

  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_entry_points();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // the main thread needs the fake TLS (TPIDR_EL0 stack-guard cookie) BEFORE
  // any game code runs -- the init_array below is game code (libc++ static
  // constructors read the cookie)
  pthr_install_fake_tls();

  so_execute_init_array(&game_mod);

  jni_init();

  // GameActivity.onCreate equivalents (paths, device, audio, asset mgr, data dir).
  // All engine symbol lookups happened in resolve_entry_points() above (before
  // so_finalize); run_boot_sequence() only CALLS them, so so_free_temp() below is
  // safe to run right after.
  run_boot_sequence();

  // all symbol lookups are done; reclaim the temporary .so image now
  so_free_temp(&game_mod);

  // we are the GLSurfaceView render thread: own EGL ourselves
  if (egl_bringup() < 0)
    fatal_error("EGL bring-up failed.");

  // onSurfaceCreated -> nativeInit (engine adopts the current EGL context);
  // onSurfaceChanged -> nativeResize; onResume -> nativeResume.
  if (g.nativeColdBoot) g.nativeColdBoot(fake_env, GLSV_OBJ);
  g.nativeInit(fake_env, GLSV_OBJ, EGLCONFIG_OBJ, ACTIVITY_OBJ);
  g.nativeResize(fake_env, GLSV_OBJ, screen_width, screen_height);
  g.nativeResume(fake_env, GLSV_OBJ);
  g.nativeWindowFocusChanged(fake_env, GLSV_OBJ, 1);
  debugPrintf("startup sequence complete\n");

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  // register the controller once so the engine's "connected" gate is set
  g.controllerSetData(fake_env, FUSION_OBJ, 0, 0, 0.0f, 0.0f);

  int last_mode = appletGetOperationMode();
  unsigned frame = 0;

  // 30 fps frame pacing: TFA advances its simulation one fixed step per rendered
  // frame, so it MUST run at 30 fps (60 fps = double speed). eglSwapInterval(2)
  // normally provides this; this cap guards against the driver not blocking the
  // full two vsyncs.
  const u64 tick_freq = armGetSystemTickFreq();
  const u64 frame_ticks = tick_freq / 30;

  // monitor thread: CPU boost only while shaders are compiling (boot + loads)
  if (R_SUCCEEDED(threadCreate(&g_monitor, monitor_fn, NULL, NULL, 0x4000, 0x2c, -2)))
    threadStart(&g_monitor);

  debugPrintf("entering render loop\n");
  while (appletMainLoop()) {
    // A cutscene presents on the engine's own thread; pause our render loop and
    // drop the shared GL context so the fmv thread can acquire it (else deadlock,
    // and interleaved black frames).
    if (g_fmv_active) {
      egl_gl_ownership_release();
      svcSleepThread(5000000ull);  // 5ms; keep pumping appletMainLoop
      continue;
    }
    const u64 frame_start = armGetSystemTick();
    update_gamepad();
    update_touch();

    g.nativeRender(fake_env, GLSV_OBJ);   // update + draw on this thread (the
                                          // first call runs Fusion_OnceInit:
                                          // save load + shader compile)
    if (frame < 8 || (frame % 600) == 0)
      debugPrintf("render: frame %u done (swaps=%d)\n", frame, egl_swap_count);
    frame++;
    egl_present();                         // wrapper presents the frame

    // dock / undock -> framebuffer resolution change
    int mode = appletGetOperationMode();
    if (mode != last_mode) {
      last_mode = mode;
      set_screen_size(config.screen_width, config.screen_height);
      egl_resize_surface(screen_width, screen_height);
      g.nativeResize(fake_env, GLSV_OBJ, screen_width, screen_height);
    }
    // CPU boost is managed by monitor_fn (boosts during load freezes)

    // hold each frame to ~1/30s so the fixed-timestep sim runs at correct speed
    const u64 elapsed = armGetSystemTick() - frame_start;
    if (elapsed < frame_ticks)
      svcSleepThread((frame_ticks - elapsed) * 1000000000ull / tick_freq);
  }

  g.nativePause(fake_env, GLSV_OBJ);
  if (g.nativeDone) g.nativeDone(fake_env, GLSV_OBJ);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}

/* config.h -- global configuration and config file handling
 *
 * LEGO Star Wars: The Force Awakens (Android, arm64) on the Switch.
 * The game lib is libProject_Douglas_HH.so (WB Games "Fusion" engine), driven
 * through the com.wbgames.LEGOgame.Fusion + GameGLSurfaceView JNI surface.
 * Unlike LSWTCS (libTTapp.so, game owns its render thread), this is the classic
 * Android GLSurfaceView model: WE own EGL and call nativeRender() each frame.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory split (see __libnx_initheap): the loaded .so image gets this fixed
// reserve, the newlib heap (game malloc + mesa host allocations) gets ALL the
// rest of the Horizon heap. libProject_Douglas_HH.so is ~9 MB; 256 MB leaves
// ample headroom for the loaded image + relocations.
#define SO_REGION_MB 256

// TFA ships a single arm64 libProject_Douglas_HH.so with a statically linked
// C++ runtime (no donor needed). v2.0.1.27 (com.wb.goog.legoswtfa).
#define SO_NAME "libProject_Douglas_HH.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

// Diagnostic build switch: writes debug.log next to the .nro. Per-line SD writes
// cost seconds of boot, so leave it off for release; define it to chase a bug.
// #define DEBUG_LOG 1

// --- Android-side identity handed to the game ------------------------------
// Fusion's nativeSetDeviceStrings takes (MODEL, PRODUCT, MANUFACTURER,
// HARDWARE) in that field order. Present a generic, well-supported GLES2 phone
// (Galaxy S8) so the engine takes its standard mobile path; the GL renderer is
// reported separately (see glGetStringHook).
#define DEVICE_MODEL        "SM-G950F"
#define DEVICE_PRODUCT      "dreamlte"
#define DEVICE_MANUFACTURER "samsung"
#define DEVICE_HARDWARE     "exynos8895"

#define ANDROID_VERSION_RELEASE "5.0.2"
#define ANDROID_SDK_INT 21   // Lollipop; the engine checks isAtLeastAPI >= 21

// Audio output buffer (frames) handed to Fusion.nativeSetAudioOutputBufferSize.
// A small power of two matching the OpenSL ES buffer.
#define AUDIO_BUF_FRAMES 256

// --- game data --------------------------------------------------------------
// v2.2.1.06 ships data as Play-Asset-Delivery packs (no OBB). The user extracts
// every pack's .../assets/ contents into GAMEDATA_DIR (relative to the game dir);
// we register it with the engine via fnOBBPackages_AddAssetDir at boot.
#define PACKAGE      "com.wb.goog.legoswtfa"
#define GAMEDATA_DIR "gamedata"   // loose extracted assets, relative to the game dir

// --- filesystem paths -------------------------------------------------------
// The engine prefixes the save/cache/write paths it is handed onto its file
// opens via "%s/%s". We hand it the Android-absolute prefixes the real app
// used; fix_path() in libc_shim.c collapses every one onto the game directory
// (the process cwd, set to the .nro's folder by the launcher).
#define SAVE_PATH   "/data/user/0/com.wb.goog.legoswtfa/files"                       // getFilesDir
#define CACHE_PATH  "/data/user/0/com.wb.goog.legoswtfa/cache"                       // getCacheDir
#define WRITE_PATH  "/storage/emulated/0/Android/data/com.wb.goog.legoswtfa/files"   // getExternalFilesDir

// Save files the engine opens under SAVE_PATH (fnaFile_SaveGame*):
#define SAVE_GAME_FILE   "savegame.dat"
#define SAVE_CONFIG_FILE "config.dat"

// Physical panel size in mm; the engine uses it for DPI-based UI/touch scaling.
// Switch handheld panel is a 6.2" 1280x720 (~237 dpi).
#define SCREEN_PHYS_W_MM 136.7f
#define SCREEN_PHYS_H_MM 76.9f

// actual render/surface size (picked at runtime from docked state)
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int language;        // -1 = follow system; else index into the lang table
  int show_fps;        // 1 = small FPS counter in the top left corner
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

// resolves config.language (or the system language when -1) to a BCP-47 tag
const char *config_locale_str(void);

#endif

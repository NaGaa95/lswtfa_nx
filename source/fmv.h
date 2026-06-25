/* fmv.h -- cutscene player hooks (see fmv.c). */
#pragma once

// Non-zero while a cutscene is playing. The main render loop checks this and
// pauses its own render+present so the cutscene player is the sole presenter.
extern volatile int g_fmv_active;

// Replacements for the engine's fnaFMV_* (hooked over their bodies in game.c).
// fmv_hook_open plays the whole movie synchronously, then fmv_hook_finished
// reports completion so the engine ends the cutscene.
void          *fmv_hook_open(const char *name, int loop, const void *a,
                             unsigned b, const char *c);
unsigned char  fmv_hook_finished(void *handle);
void           fmv_hook_close(void *handle);

#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { bool held; } Mutex;
static inline void mutexLock(Mutex *m) { assert(!m->held); m->held = true; }
static inline void mutexUnlock(Mutex *m) { assert(m->held); m->held = false; }

enum {
  HidNpadButton_A = 1 << 0, HidNpadButton_B = 1 << 1,
  HidNpadButton_X = 1 << 2, HidNpadButton_Y = 1 << 3,
  HidNpadButton_StickL = 1 << 4, HidNpadButton_StickR = 1 << 5,
  HidNpadButton_L = 1 << 6, HidNpadButton_R = 1 << 7,
  HidNpadButton_ZL = 1 << 8, HidNpadButton_ZR = 1 << 9,
  HidNpadButton_Plus = 1 << 10, HidNpadButton_Minus = 1 << 11,
  HidNpadButton_Left = 1 << 12, HidNpadButton_Up = 1 << 13,
  HidNpadButton_Right = 1 << 14, HidNpadButton_Down = 1 << 15,
};

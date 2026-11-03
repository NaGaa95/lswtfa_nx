#include <math.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>

#include "controller.h"

/* Verified against fnaController_CreateDevice and fnInput_Poll in 2.2.1.06.
 * The enclosing poll retains ownership of deadzones and button transitions. */
typedef struct {
  float value;
  uint32_t kind;
  float deadzone, threshold, press_time;
  int8_t pressed, held, released;
  uint8_t reserved;
} FusionInputValue;

typedef struct {
  uint32_t flags, type, index, reserved, value_count, padding;
  FusionInputValue *values;
} FusionInputDevice;

_Static_assert(sizeof(FusionInputValue) == 24, "Fusion input value ABI");
_Static_assert(offsetof(FusionInputDevice, value_count) == 16, "Fusion channel count ABI");
_Static_assert(offsetof(FusionInputDevice, values) == 24, "Fusion input array ABI");

static Mutex frame_lock;
static float current_values[TFA_INPUT_COUNT];
static void (*fallback_process)(void *);

static float axis(int32_t value) {
  float result = (float)value / 32767.0f;
  return fmaxf(-1.0f, fminf(1.0f, result));
}

static void stick_circle_to_square(float *x, float *y) {
  float m = fmaxf(fabsf(*x), fabsf(*y));
  if (m < 1e-6f) return;
  float scale = sqrtf(*x * *x + *y * *y) / m;
  *x = fmaxf(-1.0f, fminf(1.0f, *x * scale));
  *y = fmaxf(-1.0f, fminf(1.0f, *y * scale));
}

void tfa_controller_build_frame(uint64_t buttons, int32_t lx, int32_t ly,
                                int32_t rx, int32_t ry, TfaControllerFrame *frame) {
  static const struct { uint64_t button; unsigned channel; uint32_t android; } map[] = {
    { HidNpadButton_B, TFA_SOUTH, 0x0010 },
    { HidNpadButton_A, TFA_EAST,  0x0020 },
    { HidNpadButton_Y, TFA_WEST,  0x0040 },
    { HidNpadButton_X, TFA_NORTH, 0x0080 },
    { HidNpadButton_L, TFA_L1, 0x0400 },
    { HidNpadButton_R, TFA_R1, 0x0800 },
    { HidNpadButton_ZL, TFA_L2, 0 },
    { HidNpadButton_ZR, TFA_R2, 0 },
    { HidNpadButton_StickL, TFA_LEFT_CLICK, 0 },
    { HidNpadButton_StickR, TFA_RIGHT_CLICK, 0 },
    { HidNpadButton_Plus, TFA_START, 0x0100 },
    { HidNpadButton_Minus, TFA_SELECT, 0 },
    { HidNpadButton_Up, TFA_UP, 0x0004 },
    { HidNpadButton_Down, TFA_DOWN, 0x0008 },
    { HidNpadButton_Left, TFA_LEFT, 0x0001 },
    { HidNpadButton_Right, TFA_RIGHT, 0x0002 },
  };
  memset(frame, 0, sizeof(*frame));
  for (unsigned i = 0; i < sizeof(map) / sizeof(*map); ++i) {
    if (buttons & map[i].button) {
      frame->values[map[i].channel] = 1.0f;
      frame->android_buttons |= map[i].android;
    }
  }
  frame->values[TFA_LEFT_X] = axis(lx);
  frame->values[TFA_LEFT_Y] = axis(ly);
  frame->values[TFA_RIGHT_X] = axis(rx);
  frame->values[TFA_RIGHT_Y] = axis(ry);
  stick_circle_to_square(&frame->values[TFA_LEFT_X], &frame->values[TFA_LEFT_Y]);
  stick_circle_to_square(&frame->values[TFA_RIGHT_X], &frame->values[TFA_RIGHT_Y]);

  /* Preserve D-pad movement alongside the engine's digital D-pad channels. */
  if (buttons & HidNpadButton_Left) frame->values[TFA_LEFT_X] = -1.0f;
  if (buttons & HidNpadButton_Right) frame->values[TFA_LEFT_X] = 1.0f;
  if (buttons & HidNpadButton_Up) frame->values[TFA_LEFT_Y] = 1.0f;
  if (buttons & HidNpadButton_Down) frame->values[TFA_LEFT_Y] = -1.0f;
  frame->android_x = frame->values[TFA_LEFT_X];
  frame->android_y = -frame->values[TFA_LEFT_Y];
}

void tfa_controller_publish(const TfaControllerFrame *frame) {
  mutexLock(&frame_lock);
  memcpy(current_values, frame->values, sizeof(current_values));
  mutexUnlock(&frame_lock);
}

void tfa_controller_set_fallback(void (*process)(void *)) {
  fallback_process = process;
}

void tfa_controller_process(void *device) {
  if (!device) return;
  FusionInputDevice *joypad = device;
  if (joypad->type != 1 || joypad->value_count != TFA_INPUT_COUNT || !joypad->values) {
    if (fallback_process) fallback_process(device);
    return;
  }
  float values[TFA_INPUT_COUNT];
  mutexLock(&frame_lock);
  memcpy(values, current_values, sizeof(values));
  mutexUnlock(&frame_lock);
  for (unsigned i = 0; i < TFA_INPUT_COUNT; ++i)
    joypad->values[i].value = values[i];
}

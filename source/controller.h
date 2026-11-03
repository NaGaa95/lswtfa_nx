#ifndef LSWTFA_CONTROLLER_H
#define LSWTFA_CONTROLLER_H

#include <stdint.h>

/* Fusion's fnINPUTJOYPAD channels, not its smaller Android button mask. */
enum {
  TFA_LEFT_X, TFA_LEFT_Y, TFA_RIGHT_X, TFA_RIGHT_Y,
  TFA_LEFT_CLICK, TFA_RIGHT_CLICK, TFA_START, TFA_SELECT,
  TFA_L1, TFA_L2, TFA_R1, TFA_R2,
  TFA_UP, TFA_DOWN, TFA_LEFT, TFA_RIGHT,
  TFA_NORTH, TFA_EAST, TFA_SOUTH, TFA_WEST,
  TFA_INPUT_COUNT = 24
};

typedef struct {
  float values[TFA_INPUT_COUNT];
  uint32_t android_buttons;
  float android_x, android_y;
} TfaControllerFrame;

void tfa_controller_build_frame(uint64_t buttons, int32_t lx, int32_t ly,
                                int32_t rx, int32_t ry, TfaControllerFrame *frame);
void tfa_controller_publish(const TfaControllerFrame *frame);
void tfa_controller_set_fallback(void (*process)(void *));
void tfa_controller_process(void *device);

#endif

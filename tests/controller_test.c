#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "controller.h"

static unsigned fallback_calls;
static void *fallback_arg;
static void fallback(void *arg) { fallback_calls++; fallback_arg = arg; }

/* Independent fixture for the supplied ARM64 library's input ABI. */
typedef struct { float value; unsigned char metadata[20]; } Value;
typedef struct {
  uint32_t flags, type, index, reserved, count, padding;
  Value *values;
} Device;

static void close_float(float a, float b) { assert(fabsf(a - b) < 0.0001f); }

static void test_buttons(void) {
  static const struct { uint64_t button; unsigned slot; uint32_t native; } cases[] = {
    {HidNpadButton_B,18,0x10}, {HidNpadButton_A,17,0x20},
    {HidNpadButton_Y,19,0x40}, {HidNpadButton_X,16,0x80},
    {HidNpadButton_Plus,6,0x100}, {HidNpadButton_Minus,7,0},
    {HidNpadButton_L,8,0x400}, {HidNpadButton_R,10,0x800},
    {HidNpadButton_ZL,9,0}, {HidNpadButton_ZR,11,0},
    {HidNpadButton_StickL,4,0}, {HidNpadButton_StickR,5,0},
    {HidNpadButton_Up,12,4}, {HidNpadButton_Down,13,8},
    {HidNpadButton_Left,14,1}, {HidNpadButton_Right,15,2},
  };
  for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) {
    TfaControllerFrame f;
    tfa_controller_build_frame(cases[i].button, 0, 0, 0, 0, &f);
    assert(f.android_buttons == cases[i].native);
    for (unsigned channel = 4; channel < 24; ++channel)
      assert(f.values[channel] == (channel == cases[i].slot ? 1.0f : 0.0f));
    assert(f.values[2] == 0 && f.values[3] == 0);
    if (i < 12) assert(f.values[0] == 0 && f.values[1] == 0);
  }
  TfaControllerFrame f;
  tfa_controller_build_frame(HidNpadButton_L|HidNpadButton_R|HidNpadButton_StickL|
      HidNpadButton_StickR|HidNpadButton_Minus, 0, 0, 0, 0, &f);
  assert(f.values[6] == 0); /* stick clicks and Minus cannot pause */
  assert(f.values[12] == 0 && f.values[13] == 0 && f.values[14] == 0 && f.values[15] == 0);
}

static void test_sticks(void) {
  TfaControllerFrame f;
  tfa_controller_build_frame(0, 0, 0, 32767, -32768, &f);
  assert(f.values[0] == 0 && f.values[1] == 0);
  assert(f.values[2] == 1 && f.values[3] == -1 && f.android_buttons == 0);
  tfa_controller_build_frame(0, 32767, 32767, 0, 0, &f);
  assert(f.values[0] == 1 && f.values[1] == 1);
  assert(f.android_x == 1 && f.android_y == -1);
  tfa_controller_build_frame(0, 23170, 23170, -23170, 23170, &f);
  close_float(f.values[0],1); close_float(f.values[1],1);
  close_float(f.values[2],-1); close_float(f.values[3],1);
  tfa_controller_build_frame(0, 1000, 0, 0, 0, &f);
  close_float(f.values[0],1000.0f/32767.0f); /* engine applies the deadzone */
  tfa_controller_build_frame(HidNpadButton_Up|HidNpadButton_Left, 0, 0, 0, 0, &f);
  assert(f.values[0] == -1 && f.values[1] == 1 && f.android_y == -1);
}

static void test_poll_bridge(void) {
  Value values[25];
  memset(values,0xa5,sizeof(values));
  Device device = {.type=1,.count=24,.values=values};
  TfaControllerFrame f;
  tfa_controller_build_frame(HidNpadButton_B|HidNpadButton_ZR, 0, 0, 32767, 0, &f);
  tfa_controller_publish(&f);
  tfa_controller_process(&device);
  assert(values[18].value == 1 && values[11].value == 1 && values[2].value == 1);
  for (unsigned i=0;i<24;++i)
    for (unsigned j=0;j<20;++j) assert(values[i].metadata[j] == 0xa5);
  const unsigned char *guard = (const unsigned char *)&values[24];
  for (unsigned i=0;i<sizeof(Value);++i) assert(guard[i] == 0xa5);
  tfa_controller_build_frame(0,0,0,0,0,&f);
  tfa_controller_publish(&f); tfa_controller_process(&device);
  for (unsigned i=0;i<24;++i) assert(values[i].value == 0);

  tfa_controller_set_fallback(fallback);
  device.type=32; tfa_controller_process(&device);
  assert(fallback_calls==1 && fallback_arg==&device);
  device.type=1; device.count=23; tfa_controller_process(&device);
  assert(fallback_calls==2);
  device.count=24; device.values=NULL; tfa_controller_process(&device);
  assert(fallback_calls==3);
  tfa_controller_process(NULL); assert(fallback_calls==3);
}

/* Compile the actual relocation installer from hooks/game.c with a synthetic
 * loader image, so a changed symbol, relocation filter or ABI guard is tested. */
typedef struct { uint32_t st_name; } Elf64_Sym;
typedef struct { uint32_t sh_type; uint64_t sh_addr, sh_size; } Elf64_Shdr;
typedef struct { uint64_t r_offset,r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { int e_shnum; } Elf64_Ehdr;
typedef struct {
  void *load_base; Elf64_Ehdr *elf_hdr; Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms; char *dynstrtab;
} so_module;
#define SHT_RELA 4
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define ELF64_R_TYPE(info) ((uint32_t)(info))
#define ELF64_R_SYM(info) ((info)>>32)
static so_module game_mod;
static uint32_t entry = 0xb40018a0;
static int missing_symbol;
static uintptr_t so_try_find_addr(so_module *mod,const char *name) {
  (void)mod;
  assert(!strcmp(name,"_Z37fnaController_ProcessJoypadControllerP13fnINPUTDEVICE"));
  return missing_symbol ? 0 : (uintptr_t)&entry;
}
static uintptr_t so_try_find_addr_rx(so_module *mod,const char *name) {
  (void)mod; (void)name; return (uintptr_t)&fallback;
}
static int debugPrintf(const char *text,...) { (void)text; return 0; }
#include "controller_installer.inc"

static void test_installer(void) {
  union { uint64_t align; unsigned char bytes[512]; } image;
  memset(&image,0,sizeof(image));
  Elf64_Rela *rels = (Elf64_Rela *)&image.bytes[64];
  rels[0]=(Elf64_Rela){384,((uint64_t)1<<32)|R_AARCH64_JUMP_SLOT,0};
  rels[1]=(Elf64_Rela){392,((uint64_t)2<<32)|R_AARCH64_GLOB_DAT,0};
  rels[2]=(Elf64_Rela){400,((uint64_t)1<<32)|1027,0};
  Elf64_Shdr sections[]={{SHT_RELA,64,3*sizeof(*rels)}};
  Elf64_Ehdr header={1};
  char strings[]="\0_Z37fnaController_ProcessJoypadControllerP13fnINPUTDEVICE\0other\0";
  Elf64_Sym symbols[]={{0},{1},{sizeof(strings)-7}};
  game_mod=(so_module){image.bytes,&header,sections,symbols,strings};
  install_controller_hook();
  assert(*(uintptr_t *)&image.bytes[384] == (uintptr_t)tfa_controller_process);
  assert(*(uintptr_t *)&image.bytes[392] == 0 && *(uintptr_t *)&image.bytes[400] == 0);
  *(uintptr_t *)&image.bytes[384]=0;
  entry=0; install_controller_hook(); assert(*(uintptr_t *)&image.bytes[384] == 0);
  entry=0xb40018a0; missing_symbol=1;
  install_controller_hook(); assert(*(uintptr_t *)&image.bytes[384] == 0);
}

int main(void) {
  test_buttons(); test_sticks(); test_poll_bridge(); test_installer();
  puts("PASS controller buttons, both sticks, D-pad, release, guest ABI, metadata preservation and relocation installer");
  return 0;
}

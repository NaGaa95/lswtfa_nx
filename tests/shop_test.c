#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t st_name; } Elf64_Sym;
typedef struct { uint32_t sh_type; uint64_t sh_addr, sh_size; } Elf64_Shdr;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { int e_shnum; } Elf64_Ehdr;
typedef struct {
  void *load_base; Elf64_Ehdr *elf_hdr; Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms; char *dynstrtab;
} so_module;
#define SHT_RELA 4
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define ELF64_R_TYPE(info) ((uint32_t)(info))
#define ELF64_R_SYM(info) ((info) >> 32)
static so_module game_mod;
static const char *native_names[] = {
  "_ZN8SaveGame11SetIAPStateEmNS_9IAP_STATEE",
  "_ZN8SaveGame16setIAPPackStatusEbb",
  "_ZN9Character18GetFirstPlayerCharEv",
  "_ZN9Character17GetLastPlayerCharEv",
  "_ZN8SaveGame11SetCharDataEjNS_8CHARDATAE",
  "_ZN8SaveGame17SetRedBrickBoughtEm",
  "_ZN16FreeplayBindings26mobileShop_getFromCatShipsEj",
  "_ZN10FlightShip10IsUnlockedEj",
};
static uint32_t native_code[][4] = {
  {0xd0002e28, 0x91102108, 0x8b000d09, 0x39578129},
  {0xd10243ff, 0xa9036ffc, 0xa90467fa, 0xa9055ff8},
  {0x52800020, 0xd65f03c0},
  {0xa9bf7bfd, 0x910003fd, 0x97ff3628, 0x51000408},
  {0x0b000428, 0x90002e4a, 0x53037d09, 0x9110214a},
  {0x90002e28, 0x91102108, 0x8b400d08, 0x39507d09},
  {0xf00020a8, 0xf9479d09, 0x2a0003e8, 0xeb08013f},
  {0xf0002b28, 0xf9462508, 0xf9400109, 0x52800608},
};
static unsigned missing_rw, missing_rx;
static unsigned setter_calls, updater_calls, character_calls, red_brick_calls;
static unsigned char characters[225], red_bricks[24];
static bool last_save, last_refresh;
static int season_pass;

static void native_setter(unsigned long item, int state) {
  assert(item == 0 && state == 2);
  season_pass = state;
  ++setter_calls;
}
static void native_updater(bool save, bool refresh) {
  assert(season_pass == 2 && setter_calls == updater_calls + 1);
  assert(character_calls == setter_calls * 450 && red_brick_calls == setter_calls * 24);
  for (unsigned i = 0; i < 225; ++i) assert(characters[i] == 3);
  for (unsigned i = 0; i < 24; ++i) assert(red_bricks[i] == 1);
  last_save = save; last_refresh = refresh;
  ++updater_calls;
}
static unsigned int native_first(void) { return 1; }
static unsigned int native_last(void) { return 225; }
static void native_character(unsigned int index, int flag) {
  assert(index < 225 && (flag == 0 || flag == 1));
  characters[index] |= 1u << flag;
  ++character_calls;
}
static void native_red_brick(unsigned long item) {
  assert(item < 24);
  red_bricks[item] = 1;
  ++red_brick_calls;
}
static unsigned short native_ship(unsigned int index) {
  const unsigned short ships[] = {1, 3, 8};
  return index < 3 ? ships[index] : 0;
}
static unsigned native_index(const char *name) {
  for (unsigned i = 0; i < 8; ++i) if (!strcmp(name, native_names[i])) return i;
  assert(!"Unexpected native symbol"); return 0;
}
static uintptr_t so_try_find_addr(so_module *mod, const char *name) {
  assert(mod == &game_mod);
  unsigned index = native_index(name);
  return missing_rw & (1u << index) ? 0 : (uintptr_t)native_code[index];
}
static uintptr_t so_try_find_addr_rx(so_module *mod, const char *name) {
  assert(mod == &game_mod);
  uintptr_t addresses[] = {(uintptr_t)native_setter, (uintptr_t)native_updater,
    (uintptr_t)native_first, (uintptr_t)native_last, (uintptr_t)native_character,
    (uintptr_t)native_red_brick, (uintptr_t)native_ship, 1};
  unsigned index = native_index(name);
  return missing_rx & (1u << index) ? 0 : addresses[index];
}
static int debugPrintf(const char *format, ...) { (void)format; return 0; }
#include "shop_hook.inc"
typedef unsigned int juint;
#include "shop_jni.inc"

static void test_hook(void) {
  unsigned char *image = calloc(1, 0x4c6000);
  assert(image);
  uint32_t *ui = (uint32_t *)&image[0x4c55a0];
  *ui = 0x97f82508;
  Elf64_Rela *rels = (Elf64_Rela *)&image[64];
  rels[0] = (Elf64_Rela){384, ((uint64_t)1 << 32) | R_AARCH64_JUMP_SLOT, 0};
  rels[1] = (Elf64_Rela){392, ((uint64_t)3 << 32) | R_AARCH64_JUMP_SLOT, 0};
  rels[2] = (Elf64_Rela){400, ((uint64_t)1 << 32) | 1027, 0};
  rels[3] = (Elf64_Rela){408, ((uint64_t)2 << 32) | R_AARCH64_GLOB_DAT, 0};
  char strings[] = "\0_ZN8SaveGame16setIAPPackStatusEbb\0_ZN10FlightShip10IsUnlockedEj\0other\0";
  Elf64_Sym syms[] = {{0}, {1}, {sizeof("\0_ZN8SaveGame16setIAPPackStatusEbb")}, {sizeof(strings) - 7}};
  Elf64_Shdr sections[] = {{SHT_RELA, 64, 4 * sizeof(*rels)}};
  Elf64_Ehdr header = {1};
  game_mod = (so_module){image, &header, sections, syms, strings};
  patch_shop();
  assert(*(uintptr_t *)&image[384] == (uintptr_t)update_offline_packs);
  assert(*(uintptr_t *)&image[408] == (uintptr_t)is_shop_ship_unlocked);
  assert(*(uintptr_t *)&image[392] == 0 && *(uintptr_t *)&image[400] == 0);
  assert(*ui == 0x52800020);
  void (*update)(bool, bool) = (void (*)(bool, bool))*(uintptr_t *)&image[384];
  update(false, false);
  assert(setter_calls == 1 && updater_calls == 1 && !last_save && last_refresh);
  season_pass = 0; /* loading another profile reapplies ownership */
  memset(characters, 0, sizeof(characters));
  memset(red_bricks, 0, sizeof(red_bricks));
  update(true, true);
  assert(setter_calls == 2 && updater_calls == 2 && last_save && last_refresh);
  bool (*unlocked)(unsigned int) = (bool (*)(unsigned int))*(uintptr_t *)&image[408];
  assert(!unlocked(0) && unlocked(1) && !unlocked(2) && unlocked(8) && !unlocked(65535));

  *(uintptr_t *)&image[384] = *(uintptr_t *)&image[408] = 0;
  *ui = 0x97f82508;
  for (unsigned function = 0; function < 8; ++function) {
    for (unsigned word = 0; word < (function == 2 ? 2u : 4u); ++word) {
      native_code[function][word] ^= 1;
      patch_shop();
      assert(*(uintptr_t *)&image[384] == 0 && *(uintptr_t *)&image[408] == 0 && *ui == 0x97f82508);
      native_code[function][word] ^= 1;
    }
    missing_rw = 1u << function;
    patch_shop(); assert(*(uintptr_t *)&image[384] == 0 && *ui == 0x97f82508);
    missing_rw = 0;
    missing_rx = 1u << function;
    patch_shop(); assert(*(uintptr_t *)&image[384] == 0 && *ui == 0x97f82508);
    missing_rx = 0;
  }
  *ui = 0;
  patch_shop(); assert(*(uintptr_t *)&image[384] == 0);
  *ui = 0x97f82508;
  rels[3].r_addend = 1;
  patch_shop(); assert(*(uintptr_t *)&image[384] == 0 && *ui == 0x97f82508);
  rels[3].r_addend = 0;
  rels[3].r_info = ((uint64_t)1 << 32) | R_AARCH64_JUMP_SLOT; /* duplicate */
  patch_shop(); assert(*(uintptr_t *)&image[384] == 0 && *ui == 0x97f82508);
  rels[3].r_info = ((uint64_t)3 << 32) | R_AARCH64_JUMP_SLOT; /* missing ship hook */
  patch_shop(); assert(*(uintptr_t *)&image[384] == 0 && *ui == 0x97f82508);
  free(image);
}

static juint boolean_method(const char *name, ...) {
  va_list args;
  va_start(args, name);
  juint result = call_boolean(name, args);
  va_end(args);
  return result;
}

int main(void) {
  test_hook();
  assert(boolean_method("FromNative_hasConnectionQueryCompleted") == 1);
  assert(boolean_method("FromNative_IsItemPurchased", 0) == 0);
  assert(boolean_method("IsMusicActive") == 0);
  puts("PASS offline packs, stud characters, red bricks, ships, profile reload, ABI guards, atomic relocation hooks and billing-query completion");
  return 0;
}

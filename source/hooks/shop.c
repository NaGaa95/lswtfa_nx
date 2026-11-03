/* Offline content ownership for libProject_Douglas_HH.so v2.2.1.06. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../so_util.h"
#include "../util.h"

extern so_module game_mod;

static void (*set_iap_state)(unsigned long item, int state);
static void (*update_iap_packs)(bool save, bool refresh);
static unsigned int (*first_player_character)(void);
static unsigned int (*last_player_character)(void);
static void (*set_character_data)(unsigned int index, int flag);
static void (*set_red_brick_bought)(unsigned long item);
static unsigned short (*ship_from_catalog)(unsigned int index);

static bool is_shop_ship_unlocked(unsigned int ship) {
  if (!ship) return false;
  for (unsigned int i = 0; i < 256; ++i) {
    unsigned int candidate = ship_from_catalog(i);
    if (!candidate) break;
    if (candidate == ship) return true;
  }
  return false;
}

static void update_offline_packs(bool save, bool refresh) {
  (void)refresh;
  // Entry 0 is the Season Pass; state 2 is owned. The native updater grants
  // its full-game, mission-pack and character-pack contents using the installed
  // asset version. Refresh also handles fresh profiles whose states start at 0.
  set_iap_state(0, 2);
  unsigned int first = first_player_character();
  unsigned int last = last_player_character();
  if (last >= first && last - first < 256) {
    for (unsigned int character = first; character <= last; ++character) {
      set_character_data(character - first, 0); // unlocked
      set_character_data(character - first, 1); // bought
    }
  }
  for (unsigned long item = 0; item < 24; ++item)
    set_red_brick_bought(item);
  update_iap_packs(save, true);
}

void patch_shop(void) {
  const struct {
    const char *name;
    uint32_t entry[4];
    size_t entry_size;
  } functions[] = {
    {"_ZN8SaveGame11SetIAPStateEmNS_9IAP_STATEE",
     {0xd0002e28, 0x91102108, 0x8b000d09, 0x39578129}, 16},
    {"_ZN8SaveGame16setIAPPackStatusEbb",
     {0xd10243ff, 0xa9036ffc, 0xa90467fa, 0xa9055ff8}, 16},
    {"_ZN9Character18GetFirstPlayerCharEv", {0x52800020, 0xd65f03c0}, 8},
    {"_ZN9Character17GetLastPlayerCharEv",
     {0xa9bf7bfd, 0x910003fd, 0x97ff3628, 0x51000408}, 16},
    {"_ZN8SaveGame11SetCharDataEjNS_8CHARDATAE",
     {0x0b000428, 0x90002e4a, 0x53037d09, 0x9110214a}, 16},
    {"_ZN8SaveGame17SetRedBrickBoughtEm",
     {0x90002e28, 0x91102108, 0x8b400d08, 0x39507d09}, 16},
    {"_ZN16FreeplayBindings26mobileShop_getFromCatShipsEj",
     {0xf00020a8, 0xf9479d09, 0x2a0003e8, 0xeb08013f}, 16},
    {"_ZN10FlightShip10IsUnlockedEj",
     {0xf0002b28, 0xf9462508, 0xf9400109, 0x52800608}, 16},
  };
  uintptr_t addresses[sizeof(functions) / sizeof(*functions)];
  for (size_t i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
    uintptr_t writable = so_try_find_addr(&game_mod, functions[i].name);
    addresses[i] = so_try_find_addr_rx(&game_mod, functions[i].name);
    if (!writable || !addresses[i] ||
        memcmp((const void *)writable, functions[i].entry, functions[i].entry_size)) {
      debugPrintf("patch_shop: unsupported content ABI; skipped\n");
      return;
    }
  }
  // Shop item status normally requires the red brick's mission pickup before
  // exposing its controls. Unlock this UI check while leaving collection bits
  // and per-level pickup counters intact; the bought flag enables the extra.
  uint32_t *red_brick_ui = (uint32_t *)((uintptr_t)game_mod.load_base + 0x4c55a0u);
  if (*red_brick_ui != 0x97f82508u) { // bl SaveGame::IsRedBrickCollected@plt
    debugPrintf("patch_shop: unsupported red-brick UI; skipped\n");
    return;
  }

  set_iap_state = (void (*)(unsigned long, int))addresses[0];
  update_iap_packs = (void (*)(bool, bool))addresses[1];
  first_player_character = (unsigned int (*)(void))addresses[2];
  last_player_character = (unsigned int (*)(void))addresses[3];
  set_character_data = (void (*)(unsigned int, int))addresses[4];
  set_red_brick_bought = (void (*)(unsigned long))addresses[5];
  ship_from_catalog = (unsigned short (*)(unsigned int))addresses[6];
  uintptr_t *slots[2] = {NULL, NULL};
  for (int i = 0; i < game_mod.elf_hdr->e_shnum; ++i) {
    const Elf64_Shdr *section = &game_mod.sec_hdr[i];
    if (section->sh_type != SHT_RELA) continue;
    const Elf64_Rela *relocs = (const Elf64_Rela *)((uintptr_t)game_mod.load_base + section->sh_addr);
    for (size_t j = 0; j < section->sh_size / sizeof(*relocs); ++j) {
      unsigned type = ELF64_R_TYPE(relocs[j].r_info);
      if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
      const Elf64_Sym *symbol = &game_mod.syms[ELF64_R_SYM(relocs[j].r_info)];
      const char *name = game_mod.dynstrtab + symbol->st_name;
      unsigned int index;
      if (!strcmp(name, functions[1].name)) index = 0;
      else if (!strcmp(name, functions[7].name)) index = 1;
      else continue;
      if (slots[index] || relocs[j].r_addend) {
        debugPrintf("patch_shop: unsupported content relocations; skipped\n");
        return;
      }
      slots[index] = (uintptr_t *)((uintptr_t)game_mod.load_base + relocs[j].r_offset);
    }
  }
  if (!slots[0] || !slots[1]) {
    debugPrintf("patch_shop: missing content relocations; skipped\n");
    return;
  }
  *slots[0] = (uintptr_t)update_offline_packs;
  *slots[1] = (uintptr_t)is_shop_ship_unlocked;
  *red_brick_ui = 0x52800020u; // mov w0, #1
  debugPrintf("patch_shop: offline packs and stud shop enabled\n");
}

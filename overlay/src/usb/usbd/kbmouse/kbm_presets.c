// kbm_presets.c - Named mouse presets in their own flash sector
// SPDX-License-Identifier: Apache-2.0

#include "kbm_presets.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// FLASH REGION
// ============================================================================
// Sits one sector below the PS4 auth block, which itself sits below the two
// settings sectors and the BTstack region. Mirrors the offset arithmetic in
// ps4_auth_flash.c so the regions stack without overlapping.

#define BTSTACK_FLASH_SIZE (FLASH_SECTOR_SIZE * 2)

#if PICO_RP2350
#define _SEC_A (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - BTSTACK_FLASH_SIZE - FLASH_SECTOR_SIZE)
#else
#define _SEC_A (PICO_FLASH_SIZE_BYTES - BTSTACK_FLASH_SIZE - FLASH_SECTOR_SIZE)
#endif
#define _SEC_B       (_SEC_A - FLASH_SECTOR_SIZE)
#define _PS4_AUTH    (_SEC_B - FLASH_SECTOR_SIZE)
#define KBM_OFFSET   (_PS4_AUTH - FLASH_SECTOR_SIZE)

#define KBM_MAGIC    0x504D424BUL   // "KBMP"
#define KBM_VERSION  1

// Programmed in whole 256-byte pages, so the block is padded to 1024.
typedef struct __attribute__((packed)) {
    uint32_t     magic;
    uint8_t      version;
    uint8_t      pad[3];
    kbm_preset_t slots[KBM_PRESET_SLOTS];
    uint8_t      filler[1024 - 8 - (KBM_PRESET_SLOTS * 64)];
} kbm_block_t;

_Static_assert(sizeof(kbm_preset_t) == 64, "kbm_preset_t must be 64 bytes");
_Static_assert(sizeof(kbm_block_t) == 1024, "kbm_block_t must be 1024 bytes");

typedef struct { uint32_t offset, length; } erase_p;
typedef struct { uint32_t offset; const uint8_t* data; uint32_t length; } prog_p;

static void __no_inline_not_in_flash_func(do_erase)(void* v)
{
    erase_p* p = (erase_p*)v;
    flash_range_erase(p->offset, p->length);
}

static void __no_inline_not_in_flash_func(do_program)(void* v)
{
    prog_p* p = (prog_p*)v;
    flash_range_program(p->offset, p->data, p->length);
}

static const kbm_block_t* mapped(void)
{
    return (const kbm_block_t*)(XIP_BASE + KBM_OFFSET);
}

static bool block_valid(void)
{
    const kbm_block_t* b = mapped();
    return b->magic == KBM_MAGIC && b->version == KBM_VERSION;
}

// Read-modify-write the whole sector. A preset save is a deliberate, rare user
// action, so the simplicity is worth more than avoiding the erase.
static bool commit(const kbm_block_t* src)
{
    static kbm_block_t buf;
    memcpy(&buf, src, sizeof(buf));
    buf.magic = KBM_MAGIC;
    buf.version = KBM_VERSION;

    erase_p ep = { .offset = KBM_OFFSET, .length = FLASH_SECTOR_SIZE };
    if (flash_safe_execute(do_erase, &ep, UINT32_MAX) != PICO_OK) return false;

    prog_p pp = { .offset = KBM_OFFSET, .data = (const uint8_t*)&buf,
                  .length = sizeof(buf) };
    if (flash_safe_execute(do_program, &pp, UINT32_MAX) != PICO_OK) return false;

    return true;
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool kbm_presets_load(uint8_t slot, kbm_preset_t* out)
{
    if (slot >= KBM_PRESET_SLOTS || !out) return false;
    if (!block_valid()) return false;
    const kbm_preset_t* s = &mapped()->slots[slot];
    if (!s->used) return false;
    memcpy(out, s, sizeof(*out));
    return true;
}

bool kbm_presets_store(uint8_t slot, const kbm_preset_t* in)
{
    if (slot >= KBM_PRESET_SLOTS || !in) return false;

    static kbm_block_t work;
    if (block_valid()) memcpy(&work, mapped(), sizeof(work));
    else               memset(&work, 0, sizeof(work));

    memcpy(&work.slots[slot], in, sizeof(kbm_preset_t));
    work.slots[slot].used = 1;
    work.slots[slot].name[KBM_PRESET_NAME_LEN - 1] = '\0';
    return commit(&work);
}

bool kbm_presets_erase_slot(uint8_t slot)
{
    if (slot >= KBM_PRESET_SLOTS) return false;
    if (!block_valid()) return true;   // nothing stored, nothing to remove

    static kbm_block_t work;
    memcpy(&work, mapped(), sizeof(work));
    memset(&work.slots[slot], 0, sizeof(kbm_preset_t));
    return commit(&work);
}

bool kbm_presets_slot_used(uint8_t slot)
{
    if (slot >= KBM_PRESET_SLOTS || !block_valid()) return false;
    return mapped()->slots[slot].used != 0;
}

void kbm_presets_name(uint8_t slot, char* out, uint8_t len)
{
    if (!out || !len) return;
    out[0] = '\0';
    if (slot >= KBM_PRESET_SLOTS || !block_valid()) return;
    const kbm_preset_t* s = &mapped()->slots[slot];
    if (!s->used) return;
    snprintf(out, len, "%.*s", KBM_PRESET_NAME_LEN - 1, s->name);
}

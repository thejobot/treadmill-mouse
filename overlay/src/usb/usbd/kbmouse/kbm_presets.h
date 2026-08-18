// kbm_presets.h - Named mouse presets in their own flash sector
// SPDX-License-Identifier: Apache-2.0
//
// A complete pointer setup is feel, per-control assignments, and mode choices
// together. Storing those as one unit means a preset is a thing you can name,
// switch to, and carry: save it, unplug the dongle, plug it into any other
// machine, and the whole arrangement travels with it.
//
// This claims its own 4 KB sector rather than borrowing space inside flash_t,
// following the same pattern as ps4_auth_flash. That keeps FLASH_SCHEMA_VERSION
// untouched, so adding presets can never invalidate stored settings or the
// Bluetooth bond.

#ifndef KBM_PRESETS_H
#define KBM_PRESETS_H

#include <stdint.h>
#include <stdbool.h>

#define KBM_PRESET_SLOTS     8
#define KBM_PRESET_NAME_LEN  12
#define KBM_PRESET_ACTIONS   18

// Slot 0 is the live working configuration, written on every save so the
// dongle boots into whatever was last in use. Slots 1..7 are named presets.
#define KBM_PRESET_LIVE      0

typedef struct __attribute__((packed)) {
    char     name[KBM_PRESET_NAME_LEN];
    uint8_t  actions[KBM_PRESET_ACTIONS];
    uint16_t pointer_speed;
    uint8_t  scroll_v;
    uint8_t  scroll_h;
    uint8_t  curve;
    uint8_t  scroll_curve;
    uint8_t  precision_pct;
    uint8_t  turbo_pct;
    uint8_t  noise_floor;
    uint8_t  flags;
    uint8_t  stick_mode;
    uint8_t  fine_pct;
    uint8_t  touch_mode;
    uint8_t  touch_speed;
    uint8_t  used;
    uint8_t  dial_source;
    uint16_t dial_pct;
    uint8_t  gyro_mode;
    uint8_t  gyro_orient;
    uint8_t  gyro_gate;
    uint8_t  gyro_sens;
    uint8_t  gyro_flags;
    uint8_t  reserved[11];
} kbm_preset_t;   // 64 bytes

bool kbm_presets_load(uint8_t slot, kbm_preset_t* out);
bool kbm_presets_store(uint8_t slot, const kbm_preset_t* in);
bool kbm_presets_erase_slot(uint8_t slot);
bool kbm_presets_slot_used(uint8_t slot);
void kbm_presets_name(uint8_t slot, char* out, uint8_t len);

#endif // KBM_PRESETS_H

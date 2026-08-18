// kbmouse.c - Gamepad to mouse conversion
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith
//
// Pointer-only conversion: the right stick drives the cursor, the left stick
// scrolls, and buttons map exclusively to mouse buttons. No keystroke is ever
// emitted from any control.
//
// Three things differ from a naive stick-to-mouse mapping, and each one is
// load-bearing for how the pointer feels:
//
//   Centre calibration instead of a dead band. Sticks do not rest at 128 - the
//   unit this was written against rests at [126, 130, 128, 126]. A dead band
//   large enough to swallow that offset is also large enough to feel, as a
//   patch of travel where nothing happens. Instead the resting position is
//   learned continuously and treated as true centre, so the response starts at
//   the first millimetre while a released stick still parks at exactly zero.
//
//   Velocity integrated over real elapsed time. Stick deflection sets a speed
//   in pixels per second; motion is that speed multiplied by however long has
//   actually passed since the last report. Cursor travel is therefore identical
//   whether Bluetooth samples arrive at 60 Hz or 250 Hz, and whether the USB
//   host services the endpoint every 1 ms or every 8 ms. The alternative -
//   emitting a fixed delta per report - makes speed a function of link jitter.
//
//   Sub-pixel accumulation. Fractional pixels carry between reports instead of
//   truncating to zero, which is what makes slow movement smooth rather than
//   stepped, and what allows very low speeds to exist at all.

#include "kbmouse.h"
#include "kbm_presets.h"
#include "core/buttons.h"
#include "core/services/storage/flash.h"
#include "core/services/players/feedback.h"
#include "platform/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// ============================================================================
// TUNING STATE
// ============================================================================

static kbmouse_analog_config_t analog_config = {
    .pointer_speed  = KBMOUSE_DEFAULT_POINTER_SPEED,
    .scroll_v       = KBMOUSE_DEFAULT_SCROLL_V,
    .scroll_h       = KBMOUSE_DEFAULT_SCROLL_H,
    .curve          = KBMOUSE_DEFAULT_CURVE,
    .scroll_curve   = KBMOUSE_DEFAULT_SCROLL_CURVE,
    .precision_pct  = KBMOUSE_DEFAULT_PRECISION_PCT,
    .turbo_pct      = KBMOUSE_DEFAULT_TURBO_PCT,
    .noise_floor    = KBMOUSE_DEFAULT_NOISE_FLOOR,
    .flags          = KBMOUSE_DEFAULT_FLAGS,
    .stick_mode     = KBMOUSE_DEFAULT_STICK_MODE,
    .fine_pct       = KBMOUSE_DEFAULT_FINE_PCT,
    .touch_mode     = KBMOUSE_DEFAULT_TOUCH_MODE,
    .touch_speed    = KBMOUSE_DEFAULT_TOUCH_SPEED,
    .dial_source    = KBMOUSE_DEFAULT_DIAL_SOURCE,
    .dial_pct       = KBMOUSE_DEFAULT_DIAL_PCT,
    .gyro_mode      = KBMOUSE_DEFAULT_GYRO_MODE,
    .gyro_orient    = KBMOUSE_DEFAULT_GYRO_ORIENT,
    .gyro_gate      = KBMOUSE_DEFAULT_GYRO_GATE,
    .gyro_sens      = KBMOUSE_DEFAULT_GYRO_SENS,
    .gyro_flags     = KBMOUSE_DEFAULT_GYRO_FLAGS,
};

static uint8_t keyboard_led_state = 0;

// ============================================================================
// BUTTON MAPPING
// ============================================================================
// Pointer-only. Every entry is a mouse button; there is deliberately no table
// row that can produce a keycode. Controls not listed here do nothing at all.
//
//   R2 / Cross      left click        L1          back (button 4)
//   L2 / Circle     right click       Triangle    forward (button 5)
//   R1 / Square     middle click
//
// L3 and R3 are held modifiers rather than clicks - see the speed scaling in
// kbmouse_convert(). Stick clicks are awkward to hold and easy to trigger by
// accident while steering, which makes them poor click buttons but good
// modifiers. D-pad, Options, Create and PS are intentionally unmapped.

// One action per control slot, indexed by JP_BUTTON_* bit position. Runtime
// editable and persisted, so the whole layout is the user's rather than a
// compile-time decision.
static uint8_t action_map[KBMOUSE_SLOT_COUNT];

// Slot index constants, matching JP_BUTTON_* bit positions.
enum {
    SLOT_B1 = 0, SLOT_B2, SLOT_B3, SLOT_B4, SLOT_L1, SLOT_R1, SLOT_L2, SLOT_R2,
    SLOT_S1, SLOT_S2, SLOT_L3, SLOT_R3, SLOT_DU, SLOT_DD, SLOT_DL, SLOT_DR,
    SLOT_A1, SLOT_A2
};

static const uint8_t default_action_map[KBMOUSE_SLOT_COUNT] = {
    [SLOT_R2] = KBM_ACT_LEFT,       [SLOT_B1] = KBM_ACT_LEFT,
    [SLOT_L2] = KBM_ACT_RIGHT,      [SLOT_B2] = KBM_ACT_RIGHT,
    [SLOT_R1] = KBM_ACT_MIDDLE,     [SLOT_B3] = KBM_ACT_MIDDLE,
    [SLOT_L1] = KBM_ACT_BACK,       [SLOT_B4] = KBM_ACT_FORWARD,
    [SLOT_L3] = KBM_ACT_PRECISION,  [SLOT_R3] = KBM_ACT_TURBO,
    [SLOT_A2] = KBM_ACT_LEFT,       // touchpad click
    // D-pad, Create, Options and PS default to nothing.
};

// The map lives in custom_profile_t.reserved[], 20 bytes marked "future use"
// in the last profile slot. That avoids touching FLASH_SCHEMA_VERSION, which
// would wipe stored settings, and avoids the 8 global reserved bytes already
// spent on the feel settings. A future upstream use of that field would
// collide; nothing today reads or writes it.
#define MAP_PROFILE_SLOT   (CUSTOM_PROFILE_MAX_COUNT - 1)
#define MAP_MARKER_INDEX   18
#define MAP_MARKER_VALUE   0xA7

// Byte 19 of the same block holds device identity, which is deliberately not
// part of a preset. A sentinel rather than a bit, so that both an erased sector
// (0xFF) and a zeroed one read as "off" - the safe direction, since off is the
// arrangement already known to enumerate.
#define DEVFLAG_INDEX      19
#define DEVFLAG_PURE_MOUSE 0xA5

// ============================================================================
// CENTRE CALIBRATION
// ============================================================================

// Samples further than this from the learned centre are treated as deliberate
// movement and never pulled into the estimate. Comfortably wider than the
// worst resting offset seen (~4 counts), far narrower than an intentional push.
#define CENTRE_REST_WINDOW   14.0f

// Per-sample pull toward the observed rest position. At a ~250 Hz sample rate
// this settles in roughly ten seconds - slow enough that a stick held gently
// off-centre cannot drag the estimate with it.
#define CENTRE_TRACK_RATE    0.0004f

// Sanity bound on the seed. A stick held at connect must not be able to
// register a wildly wrong centre.
#define CENTRE_SEED_LIMIT    28.0f

// Seeding from one sample was the original approach and it is what produced
// visible drift after every re-pair. A single reading carries the axis's own
// noise, so the estimate starts a count or two out, and at the slow tracking
// rate above that error takes the better part of ten seconds to disappear -
// which is a cursor sliding across the screen while you watch it.
//
// Instead, hold still briefly and average. At roughly 250 Hz this is about a
// fifth of a second, during which the pointer deliberately does not move: a
// moment of nothing is easier to live with than a moment of wrong.
#define CENTRE_SEED_SAMPLES  48

// The window only accepts the average if the stick actually was at rest for it.
// Real resting noise is a count or two; anything wider means it was moving.
#define CENTRE_SEED_SPREAD   6.0f

// If the stick never holds still - picked up, or being pushed at the moment of
// connect - stop waiting after this many attempts and seed at the nominal
// centre. The fast correction below then cleans it up as soon as it is let go.
#define CENTRE_SEED_TRIES    12

// Residual error after seeding is corrected far faster than the long-term rate,
// but only while the stick is near centre. A deliberate push sits outside this
// band and cannot drag the estimate with it, which is the property the slow
// rate existed to protect in the first place.
#define CENTRE_FAST_RATE     0.02f
#define CENTRE_FAST_BAND     5.0f
#define CENTRE_FAST_SAMPLES  1500

typedef struct {
    float    centre;
    bool     seeded;
    // Seeding accumulator
    float    sum;
    float    lo, hi;
    uint16_t count;
    uint8_t  tries;
    uint16_t fast_left;
} axis_cal_t;

static axis_cal_t cal_lx, cal_ly, cal_rx, cal_ry;

static void cal_begin_seed(axis_cal_t* cal)
{
    cal->sum = 0.0f;
    cal->lo = 255.0f;
    cal->hi = 0.0f;
    cal->count = 0;
}

static void cal_reset(axis_cal_t* cal)
{
    cal->centre = 128.0f;
    cal->seeded = false;
    cal->tries = 0;
    cal->fast_left = 0;
    cal_begin_seed(cal);
}

static float axis_offset(axis_cal_t* cal, uint8_t raw)
{
    float v = (float)raw;

    if (!cal->seeded) {
        cal->sum += v;
        if (v < cal->lo) cal->lo = v;
        if (v > cal->hi) cal->hi = v;
        cal->count++;

        if (cal->count < CENTRE_SEED_SAMPLES) {
            return 0.0f;   // hold still: report no movement while measuring
        }

        float mean = cal->sum / (float)cal->count;
        bool still = (cal->hi - cal->lo) <= CENTRE_SEED_SPREAD;
        bool sane  = (mean > 128.0f - CENTRE_SEED_LIMIT &&
                      mean < 128.0f + CENTRE_SEED_LIMIT);

        if (still && sane) {
            cal->centre = mean;
        } else if (++cal->tries < CENTRE_SEED_TRIES) {
            cal_begin_seed(cal);
            return 0.0f;   // was moving; measure again
        } else {
            cal->centre = 128.0f;
        }

        cal->seeded = true;
        cal->fast_left = CENTRE_FAST_SAMPLES;
        return v - cal->centre;
    }

    float delta = v - cal->centre;

    if (delta > -CENTRE_REST_WINDOW && delta < CENTRE_REST_WINDOW) {
        float rate = CENTRE_TRACK_RATE;
        if (cal->fast_left > 0) {
            cal->fast_left--;
            if (delta > -CENTRE_FAST_BAND && delta < CENTRE_FAST_BAND) {
                rate = CENTRE_FAST_RATE;
            }
        }
        cal->centre += rate * delta;
    }

    return v - cal->centre;
}

// ============================================================================
// RESPONSE CURVE
// ============================================================================

// Normalised deflection -> normalised speed.
//
// Blends square and cubic: curve = 100 gives n^2, curve = 0 gives n^3. Both
// keep a wide precision region near centre while still reaching full speed at
// full deflection; the cubic end simply trades mid-range pace for finer
// control. A linear response is deliberately not offered - it makes the first
// millimetre of travel jump.
static float shape(float n, uint8_t curve)
{
    float k = (float)curve * 0.01f;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;

    // n^2 * (k + (1-k) * n)
    return n * n * (k + (1.0f - k) * n);
}

// Raw offset from centre -> signed normalised speed in the range -1..1.
static float normalise(float offset, uint8_t curve)
{
    float floor_counts = (float)analog_config.noise_floor;
    float magnitude = offset < 0.0f ? -offset : offset;

    if (magnitude <= floor_counts) return 0.0f;

    // Rescale so the first count above the floor starts from zero speed rather
    // than jumping. With the default floor of zero this is a plain divide.
    float span = 127.0f - floor_counts;
    if (span < 1.0f) span = 1.0f;

    float n = (magnitude - floor_counts) / span;
    if (n > 1.0f) n = 1.0f;

    float speed = shape(n, curve);
    return offset < 0.0f ? -speed : speed;
}

// ============================================================================
// MOTION STATE
// ============================================================================

static float vel_x, vel_y;          // pixels/second
static float vel_wheel, vel_pan;    // wheel ticks/second

static float accum_x, accum_y;      // sub-pixel carry
static float accum_wheel, accum_pan; // sub-tick carry

static uint16_t touch_prev_x, touch_prev_y;
static bool     touch_prev_valid;
static uint8_t  touch_prev_fingers;

// Consumed once by the next report, then cleared.
static float pending_touch_x, pending_touch_y;
static float pending_touch_wheel, pending_touch_pan;

static void gyro_reset(void);

static uint8_t  mouse_buttons;
static uint32_t last_report_us;
static bool     timing_primed;
static bool     recentre_held;

// ============================================================================
// CHORDS
// ============================================================================
// Hold Options and tap a face button to change mode without a computer in the
// loop. The controller buzzes on each change, because a mode switch you cannot
// see needs some acknowledgement other than the cursor behaving differently.
//
// While Options is held, no control fires its normal action. Otherwise the
// chord would click on the way past.

#define CHORD_MOD_SLOT   SLOT_S2   // Options
#define CHORD_STICK_SLOT SLOT_B1   // Cross
#define CHORD_TOUCH_SLOT SLOT_B3   // Square
#define CHORD_PRESET_SLOT SLOT_B2  // Circle

static uint32_t prev_buttons;
static uint32_t rumble_until_ms;

static void chord_buzz(uint8_t strength)
{
    feedback_set_rumble_internal(0, 0, strength);
    rumble_until_ms = platform_time_ms() + 130;
}

static void chord_buzz_task(void)
{
    if (rumble_until_ms &&
        (int32_t)(platform_time_ms() - rumble_until_ms) >= 0) {
        feedback_set_rumble_internal(0, 0, 0);
        rumble_until_ms = 0;
    }
}

// Step to the next stored preset, skipping empty slots and the live slot.
static void chord_next_preset(void)
{
    static uint8_t last = 0;
    for (uint8_t n = 1; n < KBM_PRESET_SLOTS; n++) {
        uint8_t s = (uint8_t)((last + n) % KBM_PRESET_SLOTS);
        if (s == KBM_PRESET_LIVE) continue;
        if (kbmouse_preset_load(s)) {
            last = s;
            chord_buzz(150);
            return;
        }
    }
    // Nothing saved to step through; say so with a weaker pulse.
    chord_buzz(50);
}

// Mask of every slot currently assigned to recentre, so the edge trigger
// releases correctly no matter which control it was put on.
static uint32_t recentre_mask(void)
{
    uint32_t m = 0;
    for (uint8_t s = 0; s < KBMOUSE_SLOT_COUNT; s++) {
        if (action_map[s] == KBM_ACT_RECENTRE) m |= (1u << s);
    }
    return m;
}

// Gap beyond which elapsed time is discarded rather than integrated. Covers
// first report after connect, and any stall - neither should fling the cursor.
#define MAX_INTEGRATION_US  60000u

void kbmouse_reset_config(void)
{
    analog_config.pointer_speed  = KBMOUSE_DEFAULT_POINTER_SPEED;
    analog_config.scroll_v       = KBMOUSE_DEFAULT_SCROLL_V;
    analog_config.scroll_h       = KBMOUSE_DEFAULT_SCROLL_H;
    analog_config.curve          = KBMOUSE_DEFAULT_CURVE;
    analog_config.scroll_curve   = KBMOUSE_DEFAULT_SCROLL_CURVE;
    analog_config.precision_pct  = KBMOUSE_DEFAULT_PRECISION_PCT;
    analog_config.turbo_pct      = KBMOUSE_DEFAULT_TURBO_PCT;
    analog_config.noise_floor    = KBMOUSE_DEFAULT_NOISE_FLOOR;
    analog_config.flags          = KBMOUSE_DEFAULT_FLAGS;
    analog_config.stick_mode     = KBMOUSE_DEFAULT_STICK_MODE;
    analog_config.fine_pct       = KBMOUSE_DEFAULT_FINE_PCT;
    analog_config.touch_mode     = KBMOUSE_DEFAULT_TOUCH_MODE;
    analog_config.touch_speed    = KBMOUSE_DEFAULT_TOUCH_SPEED;
    analog_config.dial_source    = KBMOUSE_DEFAULT_DIAL_SOURCE;
    analog_config.dial_pct       = KBMOUSE_DEFAULT_DIAL_PCT;
    analog_config.gyro_mode      = KBMOUSE_DEFAULT_GYRO_MODE;
    analog_config.gyro_orient    = KBMOUSE_DEFAULT_GYRO_ORIENT;
    analog_config.gyro_gate      = KBMOUSE_DEFAULT_GYRO_GATE;
    analog_config.gyro_sens      = KBMOUSE_DEFAULT_GYRO_SENS;
    analog_config.gyro_flags     = KBMOUSE_DEFAULT_GYRO_FLAGS;
}

void kbmouse_reset_map(void)
{
    memcpy(action_map, default_action_map, sizeof(action_map));
}

uint8_t kbmouse_get_action(uint8_t slot)
{
    return (slot < KBMOUSE_SLOT_COUNT) ? action_map[slot] : KBM_ACT_NONE;
}

bool kbmouse_set_action(uint8_t slot, uint8_t action)
{
    if (slot >= KBMOUSE_SLOT_COUNT || action >= KBM_ACT_COUNT) return false;
    action_map[slot] = action;
    return true;
}

void kbmouse_load_map(void)
{
    flash_t s;
    if (!flash_load(&s)) return;
    const uint8_t* r = s.profiles[MAP_PROFILE_SLOT].reserved;
    if (r[MAP_MARKER_INDEX] != MAP_MARKER_VALUE) return;   // never saved

    for (uint8_t i = 0; i < KBMOUSE_SLOT_COUNT; i++) {
        if (r[i] < KBM_ACT_COUNT) action_map[i] = r[i];
    }
}

bool kbmouse_save_map(void)
{
    flash_t s;
    if (!flash_load(&s)) return false;
    uint8_t* r = s.profiles[MAP_PROFILE_SLOT].reserved;

    for (uint8_t i = 0; i < KBMOUSE_SLOT_COUNT; i++) r[i] = action_map[i];
    r[MAP_MARKER_INDEX] = MAP_MARKER_VALUE;

    flash_save_now(&s);
    return true;
}

bool kbmouse_pure_mouse(void)
{
    flash_t s;
    if (!flash_load(&s)) return false;
    return s.profiles[MAP_PROFILE_SLOT].reserved[DEVFLAG_INDEX] == DEVFLAG_PURE_MOUSE;
}

bool kbmouse_set_pure_mouse(bool on)
{
    flash_t s;
    if (!flash_load(&s)) return false;
    s.profiles[MAP_PROFILE_SLOT].reserved[DEVFLAG_INDEX] = on ? DEVFLAG_PURE_MOUSE : 0x00;
    flash_save_now(&s);
    return true;
}

// ============================================================================
// PRESETS
// ============================================================================

static void config_to_preset(kbm_preset_t* p, const char* name)
{
    memset(p, 0, sizeof(*p));
    if (name) snprintf(p->name, KBM_PRESET_NAME_LEN, "%s", name);
    memcpy(p->actions, action_map, KBM_PRESET_ACTIONS);
    p->pointer_speed = analog_config.pointer_speed;
    p->scroll_v      = analog_config.scroll_v;
    p->scroll_h      = analog_config.scroll_h;
    p->curve         = analog_config.curve;
    p->scroll_curve  = analog_config.scroll_curve;
    p->precision_pct = analog_config.precision_pct;
    p->turbo_pct     = analog_config.turbo_pct;
    p->noise_floor   = analog_config.noise_floor;
    p->flags         = analog_config.flags;
    p->stick_mode    = analog_config.stick_mode;
    p->fine_pct      = analog_config.fine_pct;
    p->touch_mode    = analog_config.touch_mode;
    p->touch_speed   = analog_config.touch_speed;
    p->dial_source   = analog_config.dial_source;
    p->dial_pct      = analog_config.dial_pct;
    p->gyro_mode     = analog_config.gyro_mode;
    p->gyro_orient   = analog_config.gyro_orient;
    p->gyro_gate     = analog_config.gyro_gate;
    p->gyro_sens     = analog_config.gyro_sens;
    p->gyro_flags    = analog_config.gyro_flags;
    p->used          = 1;
}

static void preset_to_config(const kbm_preset_t* p)
{
    for (uint8_t i = 0; i < KBM_PRESET_ACTIONS && i < KBMOUSE_SLOT_COUNT; i++) {
        if (p->actions[i] < KBM_ACT_COUNT) action_map[i] = p->actions[i];
    }
    if (p->pointer_speed) analog_config.pointer_speed = p->pointer_speed;
    analog_config.scroll_v      = p->scroll_v;
    analog_config.scroll_h      = p->scroll_h;
    analog_config.curve         = p->curve;
    analog_config.scroll_curve  = p->scroll_curve;
    analog_config.precision_pct = p->precision_pct;
    analog_config.turbo_pct     = p->turbo_pct;
    analog_config.noise_floor   = p->noise_floor;
    analog_config.flags         = p->flags;
    analog_config.stick_mode    = (p->stick_mode < KBM_STICK_MODE_COUNT)
                                  ? p->stick_mode : KBM_STICK_RIGHT_POINTS;
    analog_config.fine_pct      = p->fine_pct;
    analog_config.touch_mode    = (p->touch_mode < KBM_TOUCH_MODE_COUNT)
                                  ? p->touch_mode : KBM_TOUCH_OFF;
    analog_config.touch_speed   = p->touch_speed;
    analog_config.dial_source   = (p->dial_source < KBM_DIAL_COUNT)
                                  ? p->dial_source : KBM_DIAL_OFF;
    analog_config.dial_pct      = p->dial_pct ? p->dial_pct : 100;
    // A preset written before the gyro existed has these bytes zeroed, which
    // would read as "always on, flat" rather than as "unset". Sensitivity is
    // never legitimately zero, so it doubles as the marker for a preset that
    // actually knows about the gyro.
    if (p->gyro_sens == 0) {
        analog_config.gyro_mode   = KBMOUSE_DEFAULT_GYRO_MODE;
        analog_config.gyro_orient = KBMOUSE_DEFAULT_GYRO_ORIENT;
        analog_config.gyro_gate   = KBMOUSE_DEFAULT_GYRO_GATE;
        analog_config.gyro_sens   = KBMOUSE_DEFAULT_GYRO_SENS;
        analog_config.gyro_flags  = KBMOUSE_DEFAULT_GYRO_FLAGS;
    } else {
        analog_config.gyro_mode   = (p->gyro_mode < KBM_GYRO_MODE_COUNT)
                                    ? p->gyro_mode : KBM_GYRO_OFF;
        analog_config.gyro_orient = (p->gyro_orient < KBM_ORIENT_COUNT)
                                    ? p->gyro_orient : KBM_ORIENT_FLAT;
        analog_config.gyro_gate   = (p->gyro_gate < KBM_GATE_COUNT)
                                    ? p->gyro_gate : KBM_GATE_HOLD;
        analog_config.gyro_sens   = p->gyro_sens;
        analog_config.gyro_flags  = p->gyro_flags;
    }
}

bool kbmouse_preset_save(uint8_t slot, const char* name)
{
    kbm_preset_t p;
    config_to_preset(&p, name);
    return kbm_presets_store(slot, &p);
}

bool kbmouse_preset_load(uint8_t slot)
{
    kbm_preset_t p;
    if (!kbm_presets_load(slot, &p)) return false;
    preset_to_config(&p);
    kbmouse_recalibrate();
    return true;
}

bool kbmouse_preset_delete(uint8_t slot)
{
    return kbm_presets_erase_slot(slot);
}

void kbmouse_preset_name(uint8_t slot, char* out, uint8_t len)
{
    kbm_presets_name(slot, out, len);
}

bool kbmouse_preset_used(uint8_t slot)
{
    return kbm_presets_slot_used(slot);
}

void kbmouse_init(void)
{
    kbmouse_reset_config();
    kbmouse_reset_map();

    // Slot 0 is the live configuration. Fall back to the older split storage
    // so a dongle saved before presets existed still comes up as it was left.
    if (!kbmouse_preset_load(KBM_PRESET_LIVE)) {
        kbmouse_load_config();
        kbmouse_load_map();
    }

    keyboard_led_state = 0;
    recentre_held = false;
    prev_buttons = 0;
    rumble_until_ms = 0;
    touch_prev_valid = false;
    gyro_reset();
    pending_touch_x = pending_touch_y = 0.0f;
    pending_touch_wheel = pending_touch_pan = 0.0f;
    kbmouse_recalibrate();
}

// ============================================================================
// PERSISTENCE
// ============================================================================
// The whole tuning set is packed into flash_t's 8 reserved bytes. That keeps
// the struct at its asserted 256 bytes and leaves FLASH_SCHEMA_VERSION alone,
// so saving mouse settings cannot invalidate stored profiles or the Bluetooth
// bond. Pointer speed is quantised to 25 px/s to fit a byte; everything else
// is already byte-sized. Bit 7 of the flags byte marks the block as written -
// an unwritten flash reads as zeros, which must mean "defaults", not "speed 0".

#define KBM_SLOT_SPEED    0
#define KBM_SLOT_SCROLL_V 1
#define KBM_SLOT_SCROLL_H 2
#define KBM_SLOT_CURVE    3
#define KBM_SLOT_SCURVE   4
#define KBM_SLOT_PRECISE  5
#define KBM_SLOT_TURBO    6
#define KBM_SLOT_FLAGS    7

void kbmouse_load_config(void)
{
    flash_t settings;
    if (!flash_load(&settings)) return;

    uint8_t flags = settings.reserved[KBM_SLOT_FLAGS];
    if (!(flags & KBMOUSE_FLAG_CONFIGURED)) return;   // never saved

    uint16_t speed = (uint16_t)settings.reserved[KBM_SLOT_SPEED] * KBMOUSE_SPEED_QUANTUM;
    if (speed > 0) analog_config.pointer_speed = speed;

    analog_config.scroll_v      = settings.reserved[KBM_SLOT_SCROLL_V];
    analog_config.scroll_h      = settings.reserved[KBM_SLOT_SCROLL_H];
    analog_config.curve         = settings.reserved[KBM_SLOT_CURVE];
    analog_config.scroll_curve  = settings.reserved[KBM_SLOT_SCURVE];
    analog_config.precision_pct = settings.reserved[KBM_SLOT_PRECISE];
    analog_config.turbo_pct     = settings.reserved[KBM_SLOT_TURBO];
    analog_config.noise_floor   = (flags & KBMOUSE_FLAG_NOISE_MASK) >> KBMOUSE_FLAG_NOISE_SHIFT;
    analog_config.flags         = flags & ~(KBMOUSE_FLAG_NOISE_MASK | KBMOUSE_FLAG_CONFIGURED);
}

bool kbmouse_save_config(void)
{
    flash_t settings;
    if (!flash_load(&settings)) return false;

    uint16_t quantised = analog_config.pointer_speed / KBMOUSE_SPEED_QUANTUM;
    if (quantised > 255) quantised = 255;
    if (quantised == 0)  quantised = 1;

    uint8_t noise = analog_config.noise_floor;
    if (noise > 7) noise = 7;

    settings.reserved[KBM_SLOT_SPEED]    = (uint8_t)quantised;
    settings.reserved[KBM_SLOT_SCROLL_V] = analog_config.scroll_v;
    settings.reserved[KBM_SLOT_SCROLL_H] = analog_config.scroll_h;
    settings.reserved[KBM_SLOT_CURVE]    = analog_config.curve;
    settings.reserved[KBM_SLOT_SCURVE]   = analog_config.scroll_curve;
    settings.reserved[KBM_SLOT_PRECISE]  = analog_config.precision_pct;
    settings.reserved[KBM_SLOT_TURBO]    = analog_config.turbo_pct;
    settings.reserved[KBM_SLOT_FLAGS] =
        (analog_config.flags & ~(KBMOUSE_FLAG_NOISE_MASK | KBMOUSE_FLAG_CONFIGURED)) |
        ((noise << KBMOUSE_FLAG_NOISE_SHIFT) & KBMOUSE_FLAG_NOISE_MASK) |
        KBMOUSE_FLAG_CONFIGURED;

    // Immediate, not flash_save(). That path is debounced and waits for the
    // Bluetooth link to go idle, so an explicit save followed by a reboot -
    // exactly what tuning looks like - silently loses the write.
    flash_save_now(&settings);
    return true;
}

void kbmouse_recalibrate(void)
{
    // Not a plain memset: the seed window tracks a running low and high, and a
    // zeroed low bound can never be beaten by a real sample, so every window
    // would look impossibly wide and the average would be thrown away.
    cal_reset(&cal_lx);
    cal_reset(&cal_ly);
    cal_reset(&cal_rx);
    cal_reset(&cal_ry);

    vel_x = vel_y = vel_wheel = vel_pan = 0.0f;
    accum_x = accum_y = accum_wheel = accum_pan = 0.0f;
    mouse_buttons = 0;
    timing_primed = false;
}

// ============================================================================
// TOUCHPAD
// ============================================================================
// The surface reports absolute finger positions, not a rate, so it feeds the
// cursor as direct displacement rather than through the velocity path. Motion
// is the change since the previous sample; the previous position is discarded
// whenever a finger lands or lifts, because the gap between one finger leaving
// and another arriving is not a movement the user made.

static void touch_update(const input_event_t* event)
{
    if (!event || !event->has_touch ||
        analog_config.touch_mode == KBM_TOUCH_OFF) {
        touch_prev_valid = false;
        touch_prev_fingers = 0;
        return;
    }

    uint8_t fingers = (event->touch[0].active ? 1 : 0) +
                      (event->touch[1].active ? 1 : 0);

    if (fingers == 0) {
        touch_prev_valid = false;
        touch_prev_fingers = 0;
        return;
    }

    uint16_t x = event->touch[0].active ? event->touch[0].x : event->touch[1].x;
    uint16_t y = event->touch[0].active ? event->touch[0].y : event->touch[1].y;

    // Finger count changing means a different contact: re-seed, do not move.
    if (!touch_prev_valid || fingers != touch_prev_fingers) {
        touch_prev_x = x;
        touch_prev_y = y;
        touch_prev_valid = true;
        touch_prev_fingers = fingers;
        return;
    }

    int32_t dx = (int32_t)x - (int32_t)touch_prev_x;
    int32_t dy = (int32_t)y - (int32_t)touch_prev_y;
    touch_prev_x = x;
    touch_prev_y = y;

    // A jump this large is a re-contact the driver reported as continuous,
    // not a real sweep. Swallow it rather than fling the cursor.
    if (dx > 400 || dx < -400 || dy > 400 || dy < -400) return;

    bool scrolling = (analog_config.touch_mode == KBM_TOUCH_SCROLL) ||
                     (fingers >= 2);

    if (scrolling) {
        // Two-finger drag scrolls, matching the platform gesture. Divided down
        // because a tick is a whole line and the pad is high resolution.
        float k = (float)analog_config.touch_speed / 900.0f;
        pending_touch_wheel += -(float)dy * k;
        pending_touch_pan   +=  (float)dx * k;
    } else {
        float k = (float)analog_config.touch_speed / 100.0f;
        pending_touch_x += (float)dx * k;
        pending_touch_y += (float)dy * k;
    }
}


// ============================================================================
// GYRO
// ============================================================================
// Wrist rotation as pointer movement, for holding the controller in one hand
// at whatever angle it ends up.
//
// Two things make this workable rather than a novelty:
//
//   Bias calibration. A gyro at rest does not read zero, and whatever it does
//   read integrates into a cursor that slides off on its own. The resting
//   reading is learned continuously, the same trick used for the stick centres:
//   samples small enough to be noise pull the estimate, samples large enough
//   to be a deliberate turn never do.
//
//   Gravity for horizontal. In world mode the accelerometer says which way is
//   down, and turning is measured about that axis. That is what makes the grip
//   irrelevant: stood on end, tipped forward, or flat, a turn to the left is
//   still a turn to the left. Local mode instead trusts a stated orientation,
//   which is cheaper but wrong the moment the controller is held differently
//   than declared.

static float gyro_bias[3];
static bool  gyro_bias_seeded;
static bool  gyro_toggle_on;
static bool  gyro_gate_held;

// Below this, in degrees/second, a reading is treated as a resting hand rather
// than an intended movement. Loose enough to catch a treadmill's jostle.
#define GYRO_REST_DEG_S    2.5f
#define GYRO_BIAS_RATE     0.0015f

static void gyro_reset(void)
{
    gyro_bias[0] = gyro_bias[1] = gyro_bias[2] = 0.0f;
    gyro_bias_seeded = false;
    gyro_toggle_on = false;
    gyro_gate_held = false;
}

// Returns pointer velocity contribution in px/s, or leaves both at zero.
static void gyro_update(const input_event_t* event, bool gate_pressed,
                        bool gate_rising, float* out_x, float* out_y)
{
    *out_x = *out_y = 0.0f;

    if (analog_config.gyro_mode == KBM_GYRO_OFF) return;
    if (!event || !event->has_motion) return;

    // Gate first, so a disabled gyro still calibrates while you walk.
    bool live;
    switch (analog_config.gyro_gate) {
        case KBM_GATE_HOLD:   live = gate_pressed; break;
        case KBM_GATE_TOGGLE: if (gate_rising) gyro_toggle_on = !gyro_toggle_on;
                              live = gyro_toggle_on; break;
        default:              live = true; break;
    }

    // Raw counts per degree/second differ by an order of magnitude between
    // pads — 1024 on a DualSense, about 16 on a Joy-Con — so the pad states
    // its own scale. Anything that does not (or predates the field) keeps the
    // Sony number, which is what this code assumed when it was written.
    float per_deg = (event->gyro_units_per_deg > 0)
                    ? (float)event->gyro_units_per_deg
                    : KBMOUSE_GYRO_PER_DEG;

    float g[3];
    for (int i = 0; i < 3; i++) {
        g[i] = (float)event->gyro[i] * (1.0f / per_deg);
    }

    if (!gyro_bias_seeded) {
        gyro_bias[0] = g[0]; gyro_bias[1] = g[1]; gyro_bias[2] = g[2];
        gyro_bias_seeded = true;
    }
    for (int i = 0; i < 3; i++) {
        float d = g[i] - gyro_bias[i];
        if (d > -GYRO_REST_DEG_S && d < GYRO_REST_DEG_S) {
            gyro_bias[i] += GYRO_BIAS_RATE * d;
        }
        g[i] -= gyro_bias[i];
    }

    if (!live) return;

    float yaw, pitch;
    if (analog_config.gyro_mode == KBM_GYRO_WORLD) {
        float a[3] = { (float)event->accel[0], (float)event->accel[1],
                       (float)event->accel[2] };
        float mag = a[0]*a[0] + a[1]*a[1] + a[2]*a[2];
        if (mag < 1.0f) return;
        mag = sqrtf(mag);
        a[0] /= mag; a[1] /= mag; a[2] /= mag;

        // Rotation about the gravity axis is the part that reads as "turning",
        // no matter how the controller is rolled or tipped.
        yaw = g[0]*a[0] + g[1]*a[1] + g[2]*a[2];
        // Up and down stays in the controller's own frame, which is what makes
        // it feel like pointing rather than steering.
        pitch = g[0] - a[0] * yaw;
    } else {
        switch (analog_config.gyro_orient) {
            case KBM_ORIENT_UPRIGHT:   yaw = g[2]; pitch = g[0]; break;
            case KBM_ORIENT_FACE_DOWN: yaw = -g[1]; pitch = -g[0]; break;
            default:                   yaw = g[1]; pitch = g[0]; break;
        }
    }

    float sens = (float)analog_config.gyro_sens;
    if (analog_config.gyro_flags & KBMOUSE_GYRO_INVERT_X) yaw = -yaw;
    if (analog_config.gyro_flags & KBMOUSE_GYRO_INVERT_Y) pitch = -pitch;

    *out_x = -yaw * sens;
    *out_y = pitch * sens;
}

void kbmouse_convert(uint32_t buttons, const profile_output_t* profile_out,
                     const input_event_t* event)
{
    chord_buzz_task();

    // --- Chords --------------------------------------------------------
    uint32_t rising = buttons & ~prev_buttons;
    prev_buttons = buttons;

    bool chord_held = (buttons >> CHORD_MOD_SLOT) & 1u;
    if (chord_held) {
        if (rising & (1u << CHORD_STICK_SLOT)) {
            analog_config.stick_mode =
                (uint8_t)((analog_config.stick_mode + 1) % KBM_STICK_MODE_COUNT);
            // The legacy swap flag would otherwise override the new choice.
            analog_config.flags &= (uint8_t)~KBMOUSE_FLAG_SWAP_STICKS;
            chord_buzz(120);
        }
        if (rising & (1u << CHORD_TOUCH_SLOT)) {
            analog_config.touch_mode =
                (uint8_t)((analog_config.touch_mode + 1) % KBM_TOUCH_MODE_COUNT);
            chord_buzz(120);
        }
        if (rising & (1u << CHORD_PRESET_SLOT)) {
            chord_next_preset();
        }
    }

    // --- Walk the assignment map ---------------------------------------
    uint8_t mb = 0;
    bool precision = false, turbo = false, gyro_gate = false;
    float btn_wheel = 0.0f, btn_pan = 0.0f;

    uint8_t dial_slot = (analog_config.dial_source == KBM_DIAL_L2) ? SLOT_L2
                      : (analog_config.dial_source == KBM_DIAL_R2) ? SLOT_R2
                      : 0xFF;

    for (uint8_t slot = 0; slot < KBMOUSE_SLOT_COUNT && !chord_held; slot++) {
        if (!(buttons & (1u << slot))) continue;
        if (slot == dial_slot) continue;   // dialing, not clicking
        switch (action_map[slot]) {
            case KBM_ACT_LEFT:    mb |= KBMOUSE_BTN_LEFT;    break;
            case KBM_ACT_RIGHT:   mb |= KBMOUSE_BTN_RIGHT;   break;
            case KBM_ACT_MIDDLE:  mb |= KBMOUSE_BTN_MIDDLE;  break;
            case KBM_ACT_BACK:    mb |= KBMOUSE_BTN_BACK;    break;
            case KBM_ACT_FORWARD: mb |= KBMOUSE_BTN_FORWARD; break;
            case KBM_ACT_PRECISION: precision = true; break;
            case KBM_ACT_TURBO:     turbo = true;     break;
            case KBM_ACT_GYRO:      gyro_gate = true; break;
            case KBM_ACT_SCROLL_UP:    btn_wheel += 1.0f; break;
            case KBM_ACT_SCROLL_DOWN:  btn_wheel -= 1.0f; break;
            case KBM_ACT_SCROLL_LEFT:  btn_pan   -= 1.0f; break;
            case KBM_ACT_SCROLL_RIGHT: btn_pan   += 1.0f; break;
            case KBM_ACT_RECENTRE:
                // Edge-triggered: recentring every sample while held would
                // drag the estimate toward wherever the stick currently is.
                if (!recentre_held) {
                    recentre_held = true;
                    kbmouse_recalibrate();
                    return;
                }
                break;
            default: break;
        }
    }
    if (!(buttons & recentre_mask())) recentre_held = false;

    mouse_buttons = mb;

    // --- Speed scaling -------------------------------------------------
    float scale = 1.0f;
    if (precision)  scale = (float)analog_config.precision_pct * 0.01f;
    else if (turbo) scale = (float)analog_config.turbo_pct * 0.01f;

    // Analog trigger as a speed dial. The trigger reports 0..255, so this
    // slides continuously between normal speed and the target rather than
    // snapping like a held modifier: a light squeeze trims a little, a full
    // squeeze commits to the extreme. Multiplies whatever the holds set, so
    // the two compose instead of fighting.
    if (analog_config.dial_source != KBM_DIAL_OFF) {
        uint8_t t = (analog_config.dial_source == KBM_DIAL_L2)
                    ? profile_out->l2_analog : profile_out->r2_analog;
        float f = (float)t * (1.0f / 255.0f);
        float target = (float)analog_config.dial_pct * 0.01f;
        scale *= (1.0f - f) + f * target;
    }

    touch_update(event);

    // --- Stick assignment ----------------------------------------------
    // The legacy swap flag still means "left points", so an older saved
    // configuration keeps behaving the way it was set.
    uint8_t mode = analog_config.stick_mode;
    if (mode >= KBM_STICK_MODE_COUNT) mode = KBM_STICK_RIGHT_POINTS;
    if (mode == KBM_STICK_RIGHT_POINTS &&
        (analog_config.flags & KBMOUSE_FLAG_SWAP_STICKS)) {
        mode = KBM_STICK_LEFT_POINTS;
    }
    bool dual  = (mode == KBM_STICK_BOTH_POINT);
    bool swap  = (mode == KBM_STICK_LEFT_POINTS);

    axis_cal_t* point_cal_x = swap ? &cal_lx : &cal_rx;
    axis_cal_t* point_cal_y = swap ? &cal_ly : &cal_ry;
    axis_cal_t* scrl_cal_x  = swap ? &cal_rx : &cal_lx;
    axis_cal_t* scrl_cal_y  = swap ? &cal_ry : &cal_ly;

    uint8_t point_raw_x = swap ? profile_out->left_x  : profile_out->right_x;
    uint8_t point_raw_y = swap ? profile_out->left_y  : profile_out->right_y;
    uint8_t scrl_raw_x  = swap ? profile_out->right_x : profile_out->left_x;
    uint8_t scrl_raw_y  = swap ? profile_out->right_y : profile_out->left_y;

    // --- Gyro ----------------------------------------------------------
    uint32_t gyro_mask = 0;
    for (uint8_t i = 0; i < KBMOUSE_SLOT_COUNT; i++) {
        if (action_map[i] == KBM_ACT_GYRO) gyro_mask |= (1u << i);
    }
    bool gyro_rising = (rising & gyro_mask) != 0;
    float gyro_vx = 0.0f, gyro_vy = 0.0f;
    gyro_update(event, gyro_gate, gyro_rising, &gyro_vx, &gyro_vy);

    // --- Pointer -------------------------------------------------------
    float nx = normalise(axis_offset(point_cal_x, point_raw_x), analog_config.curve);
    float ny = normalise(axis_offset(point_cal_y, point_raw_y), analog_config.curve);

    float pointer = (float)analog_config.pointer_speed * scale;
    // Gyro adds to the sticks rather than replacing them, so a wrist turn can
    // refine a stick push without having to let go of one to use the other.
    vel_x = nx * pointer + gyro_vx * scale;
    vel_y = ny * pointer + gyro_vy * scale;

    // Both-point mode: the second stick adds a slower vernier on the same
    // axes. Holding a coarse push with one thumb while trimming with the
    // other gives finer placement than either stick can reach alone, because
    // the fine stick's full range maps to a fraction of the speed.
    if (dual) {
        float fx = normalise(axis_offset(&cal_lx, profile_out->left_x),
                             analog_config.curve);
        float fy = normalise(axis_offset(&cal_ly, profile_out->left_y),
                             analog_config.curve);
        float fine = pointer * ((float)analog_config.fine_pct * 0.01f);
        vel_x += fx * fine;
        vel_y += fy * fine;
    }

    // --- Scroll --------------------------------------------------------
    // Y follows the HID convention (0 = up), so pushing the stick away from
    // the user gives a negative offset. Wheel positive scrolls content up,
    // which is what pushing a physical wheel forward does - hence the negate.
    // In both-point mode neither stick is free to scroll, so scroll comes only
    // from assigned buttons and the touchpad.
    float sx = 0.0f, sy = 0.0f;
    if (!dual) {
        sx = normalise(axis_offset(scrl_cal_x, scrl_raw_x), analog_config.scroll_curve);
        sy = normalise(axis_offset(scrl_cal_y, scrl_raw_y), analog_config.scroll_curve);
    }

    // Buttons assigned to scroll add to whatever the stick is already doing,
    // at the same configured rate, so a d-pad step feels like the stick.
    float wheel = (-sy + btn_wheel) * (float)analog_config.scroll_v * scale;
    float pan   = ( sx + btn_pan)   * (float)analog_config.scroll_h * scale;

    if (analog_config.flags & KBMOUSE_FLAG_INVERT_V) wheel = -wheel;
    if (analog_config.flags & KBMOUSE_FLAG_INVERT_H) pan   = -pan;
    if (analog_config.flags & KBMOUSE_FLAG_NO_HSCROLL) pan = 0.0f;

    vel_wheel = wheel;
    vel_pan   = pan;
}

static int8_t take_ticks(float* accum)
{
    // Carry the fraction; emit only whole ticks.
    int32_t whole = (int32_t)(*accum);
    *accum -= (float)whole;

    if (whole >  127) whole =  127;
    if (whole < -127) whole = -127;
    return (int8_t)whole;
}

// ============================================================================
// SELF-TEST
// ============================================================================
// Overrides the velocity the controller would otherwise set, for a bounded
// window. Lets the report path be proven end to end with nothing paired.

// Emission counters, so a missing effect on the host can be localised to
// firmware-not-sending versus host-not-acting.
static uint32_t diag_reports, diag_motion_reports;
static uint32_t diag_wheel_reports, diag_wheel_sum, diag_pan_reports;

static uint32_t test_expiry_ms;
static float    test_vx, test_vy, test_wheel, test_pan;
static uint8_t  test_buttons;

#define KBMOUSE_TEST_MAX_MS  5000

void kbmouse_test_drive(float vx, float vy, float wheel, float pan,
                        uint8_t buttons, uint32_t ms)
{
    if (ms > KBMOUSE_TEST_MAX_MS) ms = KBMOUSE_TEST_MAX_MS;

    test_vx = vx;
    test_vy = vy;
    test_wheel = wheel;
    test_pan = pan;
    test_buttons = buttons;
    test_expiry_ms = platform_time_ms() + ms;
}

static bool test_active(void)
{
    if (test_expiry_ms == 0) return false;

    // Signed comparison so wraparound of the millisecond counter cannot leave
    // a test stuck on forever.
    if ((int32_t)(platform_time_ms() - test_expiry_ms) >= 0) {
        test_expiry_ms = 0;
        test_vx = test_vy = test_wheel = test_pan = 0.0f;
        test_buttons = 0;
        return false;
    }
    return true;
}

void kbmouse_build_report(kbmouse_mouse_report_t* mouse_report)
{
    memset(mouse_report, 0, sizeof(*mouse_report));

    bool testing = test_active();
    mouse_report->buttons = testing ? test_buttons : mouse_buttons;

    uint32_t now = platform_time_us();
    uint32_t elapsed = now - last_report_us;
    last_report_us = now;

    // Discard the first interval after connect or a stall, so a long gap can
    // never be integrated into one enormous jump.
    if (!timing_primed || elapsed > MAX_INTEGRATION_US) {
        timing_primed = true;
        return;
    }

    float dt = (float)elapsed * 1e-6f;

    float ax = testing ? test_vx    : vel_x;
    float ay = testing ? test_vy    : vel_y;
    float aw = testing ? test_wheel : vel_wheel;
    float ap = testing ? test_pan   : vel_pan;

    accum_x += ax * dt;
    accum_y += ay * dt;
    accum_wheel += aw * dt;
    accum_pan   += ap * dt;

    // Touchpad contributes displacement, not velocity, so it is added once
    // and cleared rather than integrated against elapsed time.
    if (!testing) {
        accum_x += pending_touch_x;
        accum_y += pending_touch_y;
        accum_wheel += pending_touch_wheel;
        accum_pan   += pending_touch_pan;
    }
    pending_touch_x = pending_touch_y = 0.0f;
    pending_touch_wheel = pending_touch_pan = 0.0f;

    int32_t dx = (int32_t)accum_x;
    int32_t dy = (int32_t)accum_y;
    accum_x -= (float)dx;
    accum_y -= (float)dy;

    if (dx >  32767) dx =  32767;
    if (dx < -32767) dx = -32767;
    if (dy >  32767) dy =  32767;
    if (dy < -32767) dy = -32767;

    mouse_report->x = (int16_t)dx;
    mouse_report->y = (int16_t)dy;
    mouse_report->wheel = take_ticks(&accum_wheel);
    mouse_report->pan   = take_ticks(&accum_pan);

    diag_reports++;
    if (mouse_report->x || mouse_report->y) diag_motion_reports++;
    if (mouse_report->wheel) {
        diag_wheel_reports++;
        diag_wheel_sum += (mouse_report->wheel > 0) ? mouse_report->wheel
                                                    : -mouse_report->wheel;
    }
    if (mouse_report->pan) diag_pan_reports++;
}

void kbmouse_get_diag(kbmouse_diag_t* out)
{
    out->reports = diag_reports;
    out->motion_reports = diag_motion_reports;
    out->wheel_reports = diag_wheel_reports;
    out->wheel_sum = diag_wheel_sum;
    out->pan_reports = diag_pan_reports;
}

void kbmouse_clear_diag(void)
{
    diag_reports = diag_motion_reports = 0;
    diag_wheel_reports = diag_wheel_sum = diag_pan_reports = 0;
}

const kbmouse_analog_config_t* kbmouse_get_config(void)
{
    return &analog_config;
}

void kbmouse_set_config(const kbmouse_analog_config_t* config)
{
    if (config) {
        analog_config = *config;
    }
}

uint8_t kbmouse_get_led_state(void)
{
    return keyboard_led_state;
}

void kbmouse_set_led_state(uint8_t leds)
{
    keyboard_led_state = leds;
}

// Report what the calibration currently believes. Drift is invisible from the
// outside - a cursor that slides looks the same whether the centre estimate is
// wrong, a stick is being touched, or something else entirely is moving the
// pointer. This makes the estimate itself observable, so the question can be
// settled by measurement instead of argument.
void kbmouse_get_centres(kbmouse_centres_t* out)
{
    if (!out) return;
    out->lx = cal_lx.centre;   out->lx_seeded = cal_lx.seeded;
    out->ly = cal_ly.centre;   out->ly_seeded = cal_ly.seeded;
    out->rx = cal_rx.centre;   out->rx_seeded = cal_rx.seeded;
    out->ry = cal_ry.centre;   out->ry_seeded = cal_ry.seeded;
    out->settling = (cal_lx.fast_left || cal_ly.fast_left ||
                     cal_rx.fast_left || cal_ry.fast_left);
}

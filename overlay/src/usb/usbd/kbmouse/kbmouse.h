// kbmouse.h - Gamepad to mouse conversion
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith
//
// Converts gamepad input to HID mouse reports for desktop and accessibility use.
//
// This build is pointer-only: no keystroke is ever emitted from any control.
// The keyboard report type is retained so the composite descriptor and the LED
// output callback keep their existing shape, but it is always sent empty.

#ifndef KBMOUSE_H
#define KBMOUSE_H

#include <stdint.h>
#include <stdbool.h>
#include "core/services/profiles/profile.h"
#include "core/input_event.h"
#include "descriptors/kbmouse_descriptors.h"

// ============================================================================
// REPORT STRUCTURES
// ============================================================================

// Keyboard report - retained for descriptor/LED compatibility, always zeroed.
typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} kbmouse_keyboard_report_t;

// Mouse report matching the composite descriptor's 16-bit pointer interface:
// buttons(5)+pad(3) = 1 byte, X:i16, Y:i16, wheel:i8, pan:i8 = 7 bytes total.
//
// This MUST stay byte-identical to sinput_mouse_report_t. KB/Mouse mode shares
// the SInput composite descriptor (see tud_descriptor_configuration_cb in
// usbd.c), whose mouse interface declares 16-bit X/Y. Sending TinyUSB's 8-bit
// hid_mouse_report_t here yields a 5-byte report against a 7-byte descriptor,
// which the host misparses - a pure vertical delta of 3 arrives as X = 768.
typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t  wheel;
    int8_t  pan;
} kbmouse_mouse_report_t;

// ============================================================================
// MOUSE BUTTONS
// ============================================================================

#define KBMOUSE_BTN_LEFT    (1 << 0)
#define KBMOUSE_BTN_RIGHT   (1 << 1)
#define KBMOUSE_BTN_MIDDLE  (1 << 2)
#define KBMOUSE_BTN_BACK    (1 << 3)
#define KBMOUSE_BTN_FORWARD (1 << 4)

// ============================================================================
// ASSIGNABLE ACTIONS
// ============================================================================
// Every physical control gets one of these. The set is deliberately closed and
// contains no keycode: there is no value here that can produce a keystroke, so
// "this must never type" is enforced by the type rather than by discipline.

typedef enum {
    KBM_ACT_NONE = 0,
    KBM_ACT_LEFT,
    KBM_ACT_RIGHT,
    KBM_ACT_MIDDLE,
    KBM_ACT_BACK,
    KBM_ACT_FORWARD,
    KBM_ACT_PRECISION,     // Hold: pointer runs at precision_pct
    KBM_ACT_TURBO,         // Hold: pointer runs at turbo_pct
    KBM_ACT_SCROLL_UP,     // Hold: scrolls at scroll_v
    KBM_ACT_SCROLL_DOWN,
    KBM_ACT_SCROLL_LEFT,   // Hold: pans at scroll_h
    KBM_ACT_SCROLL_RIGHT,
    KBM_ACT_RECENTRE,      // Edge: relearn stick centre
    KBM_ACT_GYRO,          // Hold or toggle the gyro, per gyro_gate
    KBM_ACT_COUNT
} kbmouse_action_t;

// Control slots, in the same order as JP_BUTTON_* bit positions, so slot index
// equals bit index and the two can never drift apart.
//   0=B1 1=B2 2=B3 3=B4 4=L1 5=R1 6=L2 7=R2 8=S1 9=S2
//   10=L3 11=R3 12=DU 13=DD 14=DL 15=DR 16=A1(PS) 17=A2(touchpad click)
#define KBMOUSE_SLOT_COUNT 18

// ============================================================================
// TUNING
// ============================================================================

// Flag bits. Packed into one byte so the whole config persists inside the
// existing 8 reserved bytes of flash_t without changing its 256-byte layout
// or bumping the schema version (which would wipe pairings and profiles).
#define KBMOUSE_FLAG_INVERT_V     (1 << 0)  // Natural scrolling, vertical
#define KBMOUSE_FLAG_INVERT_H     (1 << 1)  // Natural scrolling, horizontal
#define KBMOUSE_FLAG_SWAP_STICKS  (1 << 2)  // Pointer on left stick, scroll on right
#define KBMOUSE_FLAG_NO_HSCROLL   (1 << 3)  // Suppress horizontal scroll entirely
#define KBMOUSE_FLAG_NOISE_SHIFT  4         // bits 4-6: noise floor, 0-7 raw counts
#define KBMOUSE_FLAG_NOISE_MASK   0x70
#define KBMOUSE_FLAG_CONFIGURED   (1 << 7)  // Settings written; else use defaults

// How the two sticks divide the work.
typedef enum {
    KBM_STICK_RIGHT_POINTS = 0,  // Right points, left scrolls
    KBM_STICK_LEFT_POINTS,       // Swapped
    KBM_STICK_BOTH_POINT,        // Both point: right coarse, left fine
    KBM_STICK_MODE_COUNT
} kbmouse_stick_mode_t;

// What the touchpad surface does.
typedef enum {
    KBM_TOUCH_OFF = 0,
    KBM_TOUCH_TRACKPAD,          // One finger points, two fingers scroll
    KBM_TOUCH_SCROLL,            // Any finger scrolls
    KBM_TOUCH_MODE_COUNT
} kbmouse_touch_mode_t;

// Which analog trigger, if any, dials pointer speed while you squeeze it.
// The triggers report 0..255 rather than pressed/not, so this is a continuous
// control: speed slides between normal and the target as the trigger travels,
// instead of snapping the way a held modifier does. A trigger used this way
// stops acting as a click, since reaching full squeeze would otherwise fire
// one every time you asked for the extreme of the range.
// How gyro rotation becomes cursor movement.
//
// LOCAL reads the controller's own axes and needs to be told how you are
// holding it. WORLD finds gravity with the accelerometer and measures turning
// about that, so it does not care about orientation at all: held flat, stood
// on end like a Wiimote, or face down on a treadmill, a turn is still a turn.
// WORLD is the one to use when the controller is not lying flat.
typedef enum {
    KBM_GYRO_OFF = 0,
    KBM_GYRO_LOCAL,
    KBM_GYRO_WORLD,
    KBM_GYRO_MODE_COUNT
} kbmouse_gyro_mode_t;

// Only consulted in LOCAL mode: which way the controller is turned in the hand.
typedef enum {
    KBM_ORIENT_FLAT = 0,     // Lying flat, both hands, the usual grip
    KBM_ORIENT_UPRIGHT,      // Stood on end, one hand, pointing away
    KBM_ORIENT_FACE_DOWN,    // Tipped forward, screen-side down
    KBM_ORIENT_COUNT
} kbmouse_orient_t;

// When the gyro is live. Always-on drifts with your body on a treadmill;
// holding a control is the mouse-lift equivalent, and a toggle spares your
// finger when one hand is busy.
typedef enum {
    KBM_GATE_ALWAYS = 0,
    KBM_GATE_HOLD,
    KBM_GATE_TOGGLE,
    KBM_GATE_COUNT
} kbmouse_gate_t;

#define KBMOUSE_GYRO_INVERT_X  (1 << 0)
#define KBMOUSE_GYRO_INVERT_Y  (1 << 1)

typedef enum {
    KBM_DIAL_OFF = 0,
    KBM_DIAL_L2,
    KBM_DIAL_R2,
    KBM_DIAL_COUNT
} kbmouse_dial_t;

typedef struct {
    uint16_t pointer_speed;   // Pixels/second at full stick deflection
    uint8_t  scroll_v;        // Wheel ticks/second at full deflection
    uint8_t  scroll_h;        // Pan ticks/second at full deflection
    uint8_t  curve;           // Pointer: 0 = cubic (finest) .. 100 = square
    uint8_t  scroll_curve;    // Scroll: same scale, tuned separately
    uint8_t  precision_pct;   // Speed % while the precision modifier is held
    uint8_t  turbo_pct;       // Speed % while the turbo modifier is held
    uint8_t  noise_floor;     // Raw counts ignored around calibrated centre
    uint8_t  flags;           // KBMOUSE_FLAG_*
    uint8_t  stick_mode;      // kbmouse_stick_mode_t
    uint8_t  fine_pct;        // BOTH_POINT: second stick speed, % of pointer
    uint8_t  touch_mode;      // kbmouse_touch_mode_t
    uint8_t  touch_speed;     // Pixels per 100 touchpad units
    uint8_t  dial_source;     // kbmouse_dial_t: analog trigger that scales speed
    uint16_t dial_pct;        // Speed % at full squeeze; 100 = no change
    uint8_t  gyro_mode;       // kbmouse_gyro_mode_t
    uint8_t  gyro_orient;     // kbmouse_orient_t, LOCAL mode only
    uint8_t  gyro_gate;       // kbmouse_gate_t
    uint8_t  gyro_sens;       // Pixels per degree of rotation
    uint8_t  gyro_flags;      // KBMOUSE_GYRO_INVERT_*
} kbmouse_analog_config_t;

// Speeds are per second and integrated against real elapsed time, so cursor
// travel does not change when the Bluetooth report rate or the USB service
// rate varies.
// Measured on macOS: the host applies its own pointer scaling to HID deltas,
// and because this firmware emits small deltas at ~2 kHz it sits in the
// slowest part of Apple's acceleration curve. The observed factor was a very
// consistent 0.37x (spread 0.03 across all four directions), so the commanded
// speed here is roughly a third of what reaches the screen. 3600 lands near
// 1300 px/s on screen, with R3 turbo taking it past 3000 to cross a 6K
// display in about a second. The factor varies with the host's tracking-speed
// setting, which is exactly why this is tunable rather than compensated for.
#define KBMOUSE_DEFAULT_POINTER_SPEED   3600
#define KBMOUSE_DEFAULT_SCROLL_V        14
#define KBMOUSE_DEFAULT_SCROLL_H        10
#define KBMOUSE_DEFAULT_CURVE           40
#define KBMOUSE_DEFAULT_PRECISION_PCT   30
#define KBMOUSE_DEFAULT_TURBO_PCT       250

// Scroll gets a squarer curve than the pointer. macOS has no pixel-precise
// path for a generic HID wheel - Resolution Multiplier (usage 0x48) is defined
// in Apple's usage tables and read by nothing in IOHIDFamily - so every tick is
// a whole line and the only thing that can be shaped is how often ticks fire.
// A near-cubic curve there just makes slow scrolling feel dead before it moves.
#define KBMOUSE_DEFAULT_SCROLL_CURVE    75

// Zero by default: rest position is handled by continuous centre calibration
// rather than a dead band, so there is response from the first millimetre.
#define KBMOUSE_DEFAULT_NOISE_FLOOR     0
#define KBMOUSE_DEFAULT_FLAGS           0
#define KBMOUSE_DEFAULT_STICK_MODE      KBM_STICK_RIGHT_POINTS

// In both-point mode the left stick is the vernier: same direction, a fraction
// of the speed. Coarse with one thumb, fine adjustment with the other, without
// letting go of either.
#define KBMOUSE_DEFAULT_FINE_PCT        18

// Touchpad is 1920 x 942 units across roughly 52 mm. 55 px per 100 units puts
// a full sweep at about 1000 px, close to a laptop trackpad before the host's
// own scaling.
#define KBMOUSE_DEFAULT_TOUCH_MODE      KBM_TOUCH_TRACKPAD
#define KBMOUSE_DEFAULT_TOUCH_SPEED     55

// Off by default: a trigger that quietly changes pointer speed is a surprise
// unless it was asked for. 25 means a full squeeze runs at a quarter speed.
#define KBMOUSE_DEFAULT_DIAL_SOURCE     KBM_DIAL_OFF
#define KBMOUSE_DEFAULT_DIAL_PCT        25

// Gyro off until asked for. World space is the default when it is switched on,
// because it is the one that survives being held at an angle. 25 px per degree
// puts a wrist-sized 40 degree sweep at about 1000 px before host scaling.
#define KBMOUSE_DEFAULT_GYRO_MODE       KBM_GYRO_OFF
#define KBMOUSE_DEFAULT_GYRO_ORIENT     KBM_ORIENT_UPRIGHT
#define KBMOUSE_DEFAULT_GYRO_GATE       KBM_GATE_HOLD
#define KBMOUSE_DEFAULT_GYRO_SENS       25
#define KBMOUSE_DEFAULT_GYRO_FLAGS      0

// DualSense reports 1024 units per degree/second, 8192 per g.
#define KBMOUSE_GYRO_PER_DEG            1024.0f
#define KBMOUSE_ACCEL_1G                8192.0f

// Pointer speed persists as a single byte, so it quantises. 50 px/s steps put
// the ceiling at 12750, high enough to stay useful once host scaling has taken
// its third, and the step is far finer than anyone can feel.
#define KBMOUSE_SPEED_QUANTUM           50
#define KBMOUSE_SPEED_MAX               12000
#define KBMOUSE_SPEED_MIN               100

// ============================================================================
// PUBLIC API
// ============================================================================

void kbmouse_init(void);

// Update pointer/scroll velocity and button state from one controller sample.
// Produces no motion by itself - call kbmouse_build_report() to emit.
//
// event may be NULL. When present its touchpad fields are used, which is the
// only way to reach the surface: profile_output_t carries sticks and buttons
// but no touch data.
void kbmouse_convert(uint32_t buttons, const profile_output_t* profile_out,
                     const input_event_t* event);

// Integrate current velocity over real elapsed time and fill a report. Safe to
// call at any rate, including faster than controller samples arrive.
void kbmouse_build_report(kbmouse_mouse_report_t* mouse_report);

// Discard the learned stick centre and re-seed from the next sample.
void kbmouse_recalibrate(void);

// Self-test: drive the pointer directly for a fixed duration, bypassing the
// controller entirely. The USB task emits mouse reports whenever the endpoint
// is ready, with or without anything paired, so this exercises the full report
// path - format, descriptor match, host parsing - on a machine where no
// controller is available. Speeds are px/sec and ticks/sec; ms is capped.
void kbmouse_test_drive(float vx, float vy, float wheel, float pan,
                        uint8_t buttons, uint32_t ms);

// Emission counters. Distinguishes "firmware never sent it" from "host
// ignored it" when something has no visible effect.
typedef struct {
    uint32_t reports;
    uint32_t motion_reports;
    uint32_t wheel_reports;
    uint32_t wheel_sum;
    uint32_t pan_reports;
} kbmouse_diag_t;

void kbmouse_get_diag(kbmouse_diag_t* out);
void kbmouse_clear_diag(void);

const kbmouse_analog_config_t* kbmouse_get_config(void);
void kbmouse_set_config(const kbmouse_analog_config_t* config);

// Reset tuning to compile-time defaults (does not touch flash).
void kbmouse_reset_config(void);

// Persistence. Settings live in the 8 reserved bytes of flash_t, so they
// survive reboots and travel with the dongle to any host.
void kbmouse_load_config(void);   // Called at init; no-op if never saved
bool kbmouse_save_config(void);

// Per-control assignment. slot is 0..KBMOUSE_SLOT_COUNT-1, matching
// JP_BUTTON_* bit order.
uint8_t kbmouse_get_action(uint8_t slot);
bool kbmouse_set_action(uint8_t slot, uint8_t action);
void kbmouse_reset_map(void);
void kbmouse_load_map(void);
bool kbmouse_save_map(void);

// Pure-mouse identity. Off, the device enumerates the shared SInput composite:
// gamepad, keyboard, mouse and serial. The keyboard interface is never written,
// but macOS still sees an unidentified keyboard and opens Keyboard Setup
// Assistant asking for a key that does not exist. On, the device declares one
// HID interface - a mouse - plus the serial port used for tuning, so nothing on
// the host has a keyboard to identify.
//
// This is USB identity, fixed at enumeration, so a change needs a reboot. It is
// stored outside the preset block on purpose: what the machine sees the device
// as should not change when you switch between feels.
typedef struct {
    float lx, ly, rx, ry;
    bool  lx_seeded, ly_seeded, rx_seeded, ry_seeded;
    bool  settling;          // still inside the fast-correction window
} kbmouse_centres_t;

void kbmouse_get_centres(kbmouse_centres_t* out);

bool kbmouse_pure_mouse(void);
bool kbmouse_set_pure_mouse(bool on);   // Writes flash immediately; reboot to apply

// Presets. A preset is feel plus assignments plus modes, stored as one unit in
// its own flash sector, so saving and reloading carries the whole arrangement.
// Slot 0 is the live configuration and is what the dongle boots into.
bool kbmouse_preset_save(uint8_t slot, const char* name);
bool kbmouse_preset_load(uint8_t slot);
bool kbmouse_preset_delete(uint8_t slot);
void kbmouse_preset_name(uint8_t slot, char* out, uint8_t len);
bool kbmouse_preset_used(uint8_t slot);

// Keyboard LED state (Num/Caps/Scroll Lock) from the host output report.
uint8_t kbmouse_get_led_state(void);
void kbmouse_set_led_state(uint8_t leds);

#endif // KBMOUSE_H

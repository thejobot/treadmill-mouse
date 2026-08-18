// kbmouse_mode.c - Mouse USB device mode
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Robert Dale Smith

#include "tusb.h"
#include "../usbd_mode.h"
#include "../usbd.h"
#include "../kbmouse/kbmouse.h"
#include "descriptors/kbmouse_descriptors.h"
#include "descriptors/sinput_descriptors.h"
#include "core/buttons.h"
#include <string.h>

// ============================================================================
// STATE
// ============================================================================

static kbmouse_mouse_report_t kbmouse_mouse_report;

// ============================================================================
// MODE INTERFACE IMPLEMENTATION
// ============================================================================

static void kbmouse_mode_init(void)
{
    kbmouse_init();
    memset(&kbmouse_mouse_report, 0, sizeof(kbmouse_mouse_report));
}

static bool kbmouse_mode_is_ready(void)
{
    // Pointer-only: in the shared composite a keyboard interface exists but is
    // never written, so its readiness must not gate mouse output. Requiring it
    // stalls the cursor whenever that endpoint happens to be busy. Under the
    // mouse-only identity there is no such interface at all.
    return tud_hid_n_ready(usbd_mouse_hid_itf());
}

// Emit the current motion state on the mouse interface.
//
// The report is built here rather than at conversion time so that elapsed time
// is measured against the actual send, and so that holding a stick keeps
// producing movement between controller samples.
static bool kbmouse_send_mouse(void)
{
    const uint8_t itf = usbd_mouse_hid_itf();
    if (!tud_hid_n_ready(itf)) return false;

    kbmouse_build_report(&kbmouse_mouse_report);

    // 7-byte report to match the composite descriptor's 16-bit pointer
    // interface. TinyUSB's tud_hid_n_mouse_report() helper is fixed at 8-bit
    // X/Y and would emit 5 bytes, which the host misparses: the low and high
    // halves of X are then taken from the X and Y fields, turning a small
    // vertical move into a large horizontal one.
    return tud_hid_n_report(itf, 0,
                            (const uint8_t*)&kbmouse_mouse_report,
                            sizeof(kbmouse_mouse_report));
}

static bool kbmouse_mode_send_report(uint8_t player_index,
                                      const input_event_t* event,
                                      const profile_output_t* profile_out,
                                      uint32_t buttons)
{
    (void)player_index;

    kbmouse_convert(buttons, profile_out, event);
    return kbmouse_send_mouse();
}

// Called when no fresh controller sample is pending. Motion continues from the
// velocity set by the last sample, so a held stick moves smoothly at the USB
// service rate instead of stepping once per Bluetooth report.
bool kbmouse_mode_send_idle_mouse(void)
{
    return kbmouse_send_mouse();
}

static void kbmouse_mode_handle_output(uint8_t report_id, const uint8_t* data, uint16_t len)
{
    // Keyboard LED output report (1 byte).
    // bit 0 = NumLock, bit 1 = CapsLock, bit 2 = ScrollLock
    // In composite mode, report_id is 0 (no report IDs in standalone descriptors).
    (void)report_id;
    if (len >= 1) {
        kbmouse_set_led_state(data[0]);
    }
}

static const uint8_t* kbmouse_mode_get_device_descriptor(void)
{
    // Share SInput device descriptor (same composite USB device)
    return (const uint8_t*)&sinput_device_descriptor;
}

static const uint8_t* kbmouse_mode_get_config_descriptor(void)
{
    // Composite config descriptor is built in usbd.c (desc_configuration_sinput)
    return NULL;
}

static const uint8_t* kbmouse_mode_get_report_descriptor(void)
{
    // Not used - composite mode routes by interface in tud_hid_descriptor_report_cb
    return NULL;
}

// ============================================================================
// MODE EXPORT
// ============================================================================

const usbd_mode_t kbmouse_mode = {
    .name = "KB/Mouse",
    .mode = USB_OUTPUT_MODE_KEYBOARD_MOUSE,

    .get_device_descriptor = kbmouse_mode_get_device_descriptor,
    .get_config_descriptor = kbmouse_mode_get_config_descriptor,
    .get_report_descriptor = kbmouse_mode_get_report_descriptor,

    .init = kbmouse_mode_init,
    .send_report = kbmouse_mode_send_report,
    .is_ready = kbmouse_mode_is_ready,

    .handle_output = kbmouse_mode_handle_output,
    .get_rumble = NULL,
    .get_feedback = NULL,
    .get_report = NULL,

    .get_class_driver = NULL,  // Uses built-in HID class driver
    .task = NULL,
};

# How it works, and why it had to be firmware

The goal was a cursor while walking on a treadmill, on whatever computer was in
front of it — including machines where nothing can be installed and no
permission can be granted. That last part rules out every host-side solution.
An app that maps a gamepad to the cursor needs to be installed, needs
accessibility permission on macOS, and needs doing again on the next machine.
A device that is already a mouse needs none of that, because the host has
supported mice since before it was booted.

So the work is all on the far side of the USB port.

## Why a build was unavoidable

joypad-os already has a Keyboard/Mouse output mode. Three things made it
unusable as a mouse, none of them reachable from configuration.

The stock map is a hardcoded FPS layout: the left stick types WASD, the face
buttons type Space, E, R and Q, the d-pad types 1 to 4. On a desktop that is a
keyboard which happens to also move a cursor.

`kbmouse_set_config` is never called anywhere in the upstream tree, so deadzone,
sensitivity and scroll speed are compile-time constants.

And the mouse output was malformed.

## The report-format bug

Keyboard/Mouse mode shares the SInput composite's descriptor. The mouse
interface in that descriptor declares 16-bit X and Y, which is a seven-byte
report. But `kbmouse_mode.c` sent it through TinyUSB's `tud_hid_n_mouse_report()`,
which is fixed at 8-bit axes and emits five bytes.

The host reads the bytes it was promised. The intended Y arrives as the high
byte of X. A small nudge upward becomes a large jump sideways, and the cursor is
useless in a way that looks like a sensitivity problem and is not.

Fixed by building the seven-byte struct and sending it with `tud_hid_n_report`.
The absence of cross-axis leakage in the tests below is the direct evidence
that it is fixed: a pure vertical drive producing exactly zero horizontal
movement is precisely what the old code could not do.

This is an upstream bug and has not been reported yet.

## Three things that make it feel right

**Centre calibration instead of a dead band.** The sticks on the pad used here
rest at 126, 130, 128, 126 against a nominal 128. A dead band wide enough to
swallow that is wide enough to feel like a patch of travel where nothing
happens. Instead the resting position is learned continuously: samples within 14
counts of the current estimate pull it gently, with a time constant around ten
seconds, so deliberate movement can never drag it. The result is response from
the first millimetre and a released stick that parks at exactly zero.

**Velocity integrated over real elapsed time.** Deflection sets a speed in
pixels per second, and movement is that speed times however long actually passed
since the last report, read from `platform_time_us()`. Travel is the same
whether Bluetooth samples arrive at 60 Hz or 250 Hz. The stock code emitted a
fixed delta per report, which makes cursor speed a function of link jitter —
and, because idle reports repeat the last delta at USB rate, multiplies it by an
uncontrolled factor.

**Sub-pixel accumulation.** Fractional pixels carry between reports instead of
truncating away. That is what lets very slow movement exist at all.

The 16-bit report also removes the old ±127 per-report ceiling, so fast movement
is smooth rather than clipped.

## Scroll, and why it steps

macOS has no pixel-precise path for a generic HID wheel. The HID spec's answer
is the Resolution Multiplier, usage 0x48, which is what Windows and Linux use to
accept fractions of a detent — Windows counts 120 units per click.

macOS does not implement it. Apple's IOHIDFamily was cloned and searched in
full: across the entire 9.6 MB driver stack, `ResolutionMultiplier` occurs
exactly once, as a constant definition in `IOHIDUsageTables.h`, read by nothing.
Implementing it would change nothing on a Mac.

(Worth knowing if you go looking yourself: macOS `grep` silently matches nothing
in those files, because Apple's headers are ISO-8859 rather than UTF-8 and get
treated as binary. `LC_ALL=C grep -a` is required. An earlier pass here produced
a confident false negative for exactly that reason.)

So every wheel tick is one whole line, and the only thing firmware can shape is
how often ticks fire. That is why the scroll curve defaults to 75 while the
pointer sits at 40. A near-cubic response is right for a pointer; on a quantised
wheel it just makes slow scrolling feel dead before anything moves.

## The host takes a third

macOS applies its own pointer scaling to incoming HID deltas. Because this
firmware emits small deltas at roughly 2 kHz, it sits in the slowest part of
Apple's acceleration curve. The measured factor was 0.37x — remarkably linear,
only 0.03 of spread across four directions. Four hundred commanded pixels arrive
as about 147.

That is why the default pointer speed is 3600 rather than the ~1300 you actually
want on screen. The factor depends on the host's own tracking-speed setting,
which is why it is exposed as a tunable rather than silently compensated for.

## Storage

The tuning set is packed into the 8 reserved bytes of `flash_t`. That keeps the
struct at its statically-asserted 256 bytes and leaves `FLASH_SCHEMA_VERSION`
alone, so saving mouse settings cannot invalidate stored profiles or the
Bluetooth bond. Pointer speed quantises to 50 px/s to fit a byte. Bit 7 of the
flags byte marks the block as written, because an unwritten flash reads as zeros
and that has to mean "defaults", not "speed zero".

Presets live in their own flash sector, separately from everything else.

Saving uses `flash_save_now`, not `flash_save`. The latter is debounced and
waits for the Bluetooth link to go idle, so a save followed by a reboot — which
is exactly what tuning looks like — silently loses the write. That was caught by
testing persistence rather than assuming it: the first implementation reported
`ok` and lost everything.

## The mouse-only identity

Behaviour and identity are different things. The firmware never writes a
keystroke, but the shared SInput composite still declares a keyboard interface,
and identity is what the host inspects. On macOS that means Keyboard Setup
Assistant opens on first plug, announces that the device cannot be identified,
and asks you to press the key next to left Shift. There is no such key.

Turning on the mouse-only identity declares one HID interface carrying the same
mouse report descriptor, plus the CDC serial port used for tuning. No keyboard,
no gamepad. It is a stored setting rather than a separate build, so there is
nothing to swap by hand:

```
{"cmd":"MOUSE.PURE"}          -> {"stored":0,"active":0,"reboot":false}
{"cmd":"MOUSE.PURE","on":1}   -> stores it and reboots
```

`stored` is what the next boot will use and `active` is what the host was told
at the last enumeration; they differ exactly between setting it and rebooting.
Default is off, so flashing changes nothing until it is turned on.

The flag lives in byte 19 of the map block, the one byte that block had spare,
deliberately outside the preset block — what the machine thinks the device is
should not change when you switch between feels. It is a sentinel value (0xA5)
rather than a bit, so both an erased sector (0xFF) and a zeroed one read as off,
which is the direction already known to enumerate.

Two things are easy to get wrong here. TinyUSB's `tud_hid_n_*` index is the HID
*instance*, assigned in descriptor order, not the interface number — the mouse
is instance 2 in the composite and instance 0 alone, so `kbmouse_mode.c` asks
`usbd_mouse_hid_itf()` rather than assuming. And interface 0 is the gamepad in
the composite but the mouse here, so `tud_hid_get_report_cb` and
`tud_hid_set_report_cb` both need an early return, or mouse traffic routes into
the SInput handler.

There is a second, unavoidable dialog on Apple Silicon: "Allow accessory to
connect?" is macOS's own USB accessory policy. It fires for any new device and
no firmware can suppress it.

## Diagnosing

`MOUSE.DIAG` reports what the firmware actually emitted — total reports, and how
many carried motion, wheel or pan. It separates "firmware never sent it" from
"host ignored it", which is the distinction that turns a scroll investigation
from guesswork into a two-minute answer.

```
./tools/jp.py MOUSE.DIAG clear=1
./tools/jp.py MOUSE.TEST wheel=30 ms=2000
./tools/jp.py MOUSE.DIAG
   -> reports 4269, wheel_reports 60, wheel_sum 60
```

Exactly 60 ticks in two seconds at 30 ticks/s. Firmware correct — so on that
occasion the fault was in the measurement.

### Two measurement traps on macOS, both hit here

`CGEventSourceCounterForEventType` and `CGEventSourceSecondsSinceLastEventType`
take a state ID. With `kCGEventSourceStateHIDSystemState` (1), scroll and
other-button events read as never having happened — it reported a last scroll
four days ago on a machine scrolled daily. Use
`kCGEventSourceStateCombinedSessionState` (0). Under state 1 this firmware
looked like it had broken scroll; under state 0 the same firmware logs 130
scroll events per burst.

And Mac Mouse Fix, if installed, intercepts middle-click and the back button
before the session counter sees them. That is host-side, and it is indirect
evidence the buttons work, since it can only remap what the HID report
delivered. The self-test therefore asserts on the left button, which is
unambiguous.

## What was measured

With no controller attached, using `tools/selftest.py`, which drives the report
path from firmware via `MOUSE.TEST`:

```
POINTER
  right  commanded   +400 px   observed    +147 px   cross-axis   +0   ratio 0.37
  left   commanded   -400 px   observed    -143 px   cross-axis   +0   ratio 0.36
  down   commanded   +300 px   observed    +113 px   cross-axis   +0   ratio 0.38
  up     commanded   -300 px   observed    -117 px   cross-axis   +0   ratio 0.39
SCROLL
  wheel 30 ticks/s for 1.5s  ->  host logged 130 scroll events
  pan   30 ticks/s for 1.5s  ->  host logged 130 scroll events
BUTTONS
  left button held 0.25s     ->  host logged 1 down, 1 up
```

With a real DualSense over Bluetooth, hands off the trackpad throughout, using
`tools/record_test.py` and `tools/click_test.py`:

```
controller samples             : 12659
cursor travel, R stick pushed  : 49062 px (595 samples)
cursor travel, both centred    :   472 px (753 samples)
scroll events while L pushed   :   743    (541 samples)
left-button (R2) down/up       :    15 / 15
```

Cursor movement is attributed only to samples where the stick was measurably
deflected. Cursor travel on its own proves nothing: it cannot tell a dongle from
a hand on the trackpad, which is a mistake made earlier in this project and
worth not repeating.

### The deadzone question, settled

With the controller genuinely untouched — face down, hands off — the right stick
reads a steady 128/127 with a standard deviation of 0.00, and the cursor
recorded 0.0 px of travel over 12 seconds with the noise floor at zero. Response
from the first millimetre and no creep at all. The continuous centre calibration
does the whole job.

An earlier measurement appeared to show 142 px/s of jitter. That was a hand
resting on the stick: the same capture showed the right stick swinging across
nearly its full range while the left stick sat perfectly still, which is not
what noise looks like. A rest baseline is only valid if the controller is
actually untouched.

### A known gap in the test tools

`record_test.py` reports R2 as NOT TRIED even when R2 works. Button state lives
only in the stream's verbose packets, which arrive too rarely to sample; the
compact packets carry axes alone. `click_test.py` proves clicks by isolation
instead — press only R2, touch nothing else, and every click counted must have
come from the dongle.

## Deliberately not done

Screen zoom and desktop gestures. Both need keyboard output, and both need
per-machine settings that do not travel. Both cost the one property that makes
this safe to plug into a machine you do not administer.

Spotlight on the PS button, for the same reason — a real Cmd+Space means keeping
the keyboard interface. The only version worth trying is the consumer-control
search key, which may or may not open Spotlight on macOS, and it is unconfirmed
whether the PS button even reaches the firmware as a mappable button rather than
being swallowed by pairing and power handling.

## Still to do

Diagonals. Each axis is normalised separately, so a full diagonal runs about 40%
fast. JoyShockMapper normalises stick magnitude instead, which is the right fix.

Then an acceleration ramp, and per-stick modes rather than the fixed three-way
enum.

And reporting the report-format bug upstream.

# Controls

## The button on the board

The Pico's own BOOTSEL button does four things, told apart by how you press it.

| Press | What happens |
|---|---|
| Click | Start a 60-second Bluetooth scan, so a controller can pair |
| Double-click | Step to the next USB output mode |
| Triple-click | Drop into the UF2 bootloader to take a new firmware file |
| Hold ~1.5s | Disconnect everything and forget all pairings |

The output modes cycle SInput, XInput, PS3, PS4, Switch, Keyboard/Mouse, and
back to SInput. A freshly flashed board starts at SInput, so five double-clicks
gets you to the mouse. The mode is saved, so it only has to be done once.

That button is not a normal GPIO — reading it stops the flash and disables
interrupts for a few microseconds each time, so it is sampled at 20 Hz rather
than continuously. The multi-click window is stretched to 700 ms to compensate,
which is what makes a triple-click land reliably instead of falling between
samples. Raising the sample rate instead was tried and stopped the board
enumerating over USB at all.

## The controller, in mouse mode

Defaults. Every one of the eighteen assignments can be changed.

```
Right stick    cursor
Left stick     scroll, vertical and horizontal

R2 / Cross     left click
L2 / Circle    right click
R1 / Square    middle click
L1             back              Triangle    forward

L3 (hold)      precision, 30% speed
R3 (hold)      turbo, 250% speed

D-pad, Options, Create, PS     nothing
```

## Chords, for changing mode with no computer

Hold Options and tap a face button. The controller buzzes to confirm, because a
mode change you cannot see needs some acknowledgement other than the cursor
behaving differently. While Options is held, no control fires its normal
action — otherwise the chord would click on the way past.

| Chord | What it cycles |
|---|---|
| Options + Cross | Stick mode: right points, left points, both point |
| Options + Square | Touchpad: trackpad, scroll only, off |
| Options + Circle | Step to the next saved preset |

A weak pulse on the preset chord means there is nothing saved to step to.

## The modes those chords cycle

Stick mode. Right points and left scrolls is the default. Swapped is the same
thing the other way round. Both point puts a coarse pointer on the right stick
and a fine one on the left at 18% speed, which reaches placements neither stick
manages alone.

Touchpad. As a trackpad, one finger points and two fingers scroll. As scroll
only, any finger scrolls and nothing points. Off is off.

Presets. Eight slots in their own flash sector, each holding the feel, all
eighteen assignments and every mode. Slot 0 is what the dongle boots into.
They are kept separately from the identity setting on purpose: what the machine
thinks the device is should not change when you switch between feels.

## Modes that are off by default

The speed dial puts a continuous speed control on L2 or R2, sliding rather than
switching at a threshold, and moves that trigger's click up to the bumper.

Gyro has three settings. Off. Local, where the pad's own axes steer the cursor,
which is the usual behaviour and depends on how you are holding it. World,
where the accelerometer is used to find gravity, so the grip angle stops
mattering — that is the one that suits walking, since a hand on a treadmill
rail is not held at any fixed angle. Bias is calibrated continuously either way
so the cursor does not wander.

Both are reachable from `config.html` or over the serial port. Neither has been
through an instrumented test on hardware; the gyro axis signs in particular are
reasoned rather than observed, so expect at least one to need flipping. There
are invert switches for exactly that.

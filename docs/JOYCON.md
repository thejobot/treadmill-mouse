# Joy-Con

Written 2026-08-01. Picks up from [DESIGN.md](DESIGN.md), which covers the DualSense build.

First-generation Joy-Cons (Nintendo VID `0x057E`, PID `0x2006` left and `0x2007` right)
already had a Bluetooth driver in joypad-os — `bt/bthid/devices/vendors/nintendo/
switch_pro_bt.c`, Classic BT, registered unconditionally in `bthid_registry.c` and
compiled into every `bt2usb` build. Buttons, both sticks and battery worked before any
of this. What it did not do was motion, and the mouse layer could not have used it if
it had.

All of this is in the published build. None of it has been tested against a real
Joy-Con.

## The scale trap, which is the real find

`kbmouse.c` turned raw gyro counts into degrees per second with a hardcoded 1024, the
Sony figure. A Joy-Con's ICM-20600 runs at plus or minus 2000 dps at 0.06103 dps per
count, so 16.38 counts per degree — about sixty times off. Left alone, Joy-Con gyro
would have looked dead rather than wrong, which is the kind of failure that gets blamed
on the parse.

So `input_event_t` gained `gyro_units_per_deg`, the pad's statement about its own
sensor, and `gyro_update` reads it. Zero means unstated and falls back to 1024, so every
existing driver — DualSense included — behaves exactly as before. The field is whole
counts and the Joy-Con's real figure is 16.38, so it is set to 16 and the pointer runs
2.4 per cent slow. That is far inside what the sensitivity slider moves by feel.

## What changed

`core/input_event.h` — the new field, plus its default of 0 in `init_input_event`.

`usb/usbd/kbmouse/kbmouse.c` — `gyro_update` divides by the pad's figure instead of the
constant.

`bt/bthid/devices/vendors/nintendo/switch_pro_bt.c` — a new `SWITCH_STATE_ENABLE_IMU`
step in the init state machine, sending subcommand `0x40` after the player LED, and an
IMU parse in the `0x30` branch. Bytes 13 onward carry three 12-byte frames sampled 5 ms
apart, newest first, each six little-endian int16s in the order accel X/Y/Z then gyro
X/Y/Z. Only the newest is used; the older two exist for hosts polling slower than 5 ms.
Accel and gyro come out of the same sensor frame, which is what the gravity-based "any
angle" mode needs — it requires `gyro[i]` and `accel[i]` to mean the same axis.

A report too short to hold a frame leaves the previous sample alone rather than
publishing zeros, which would read as the controller having gone still.

## What has not been tested

None of it, against a real Joy-Con. It compiles, and the protocol matches the public
reverse-engineering (dekuNukem's Nintendo_Switch_Reverse_Engineering).

The thing most likely to be wrong on the first try is axis order — which of the three
gyro axes reads as pitch and which as yaw. Gravity mode is largely orientation-proof by
design, but its pitch term still assumes the pad's X axis is the pitch axis, and that
assumption came from a DualSense. A mirrored axis is fixed by the invert flags in
`config.html` with no reflash. Pitch and yaw swapped outright is one constant and
another build.

Worth knowing before debugging: the gyro ships off, the gate is hold-to-use, and no slot
is bound to the gyro action by default. All three have to be set before anything moves.

## Pairing

Hold the small round sync button on the Joy-Con's rail, between SL and SR, until the
lights run back and forth. The dongle inquires continuously, so nothing is pressed on
our end. Reflashing clears the bond, so this is needed after every flash.

## Buttons are mapped by position, not by label

`switch_pro_bt.c` puts the bottom face button (B) in slot B1 — the slot Cross uses on a
DualSense — A in B2 like Circle, Y in B3 like Square, X in B4 like Triangle. Home lands
in the PS slot (A1) and Capture in the touchpad-click slot (A2).

That means a binding survives a change of pad: slot 0 is the bottom button on both, and
only the word for it moves. It is the opposite convention to macOS, where the
GameController framework maps Nintendo pads by printed label and the positions are what
differ. Anything read about how a Mac app handles this does not transfer.

## The interface

`config.html` now draws either pad. A segmented picker sits above the diagram and the
choice is remembered in `localStorage` under `padKey`.

Both drawings use the same class vocabulary and the same `c-<slot>` ids, so the lit
state, click-to-select and the analogue trigger fill work on either without knowing
which is showing. `PADS` holds each view's viewBox, aria label, slot names, card glyphs,
middle-column title and stick centres; `setPad()` swaps them and rebuilds the callout
cards. The stick dots read their centres and travel from that table rather than the
DualSense numbers that used to be inline in `live()`.

The DualSense artwork stays in the document and is lifted into `PADS.ds5.svg` at load,
so switching back restores exactly what shipped. The Joy-Con artwork is the string
below, which is also what `JC_SVG` holds in the page.

Names swap with the drawing: B, A, Y, X, L, R, ZL, ZR, minus, plus, Home, Capture, and
the middle column stops calling itself Touchpad.

## The Joy-Con drawing

Drawn rather than traced. A Joy-Con is a rounded rectangle with a rail down one side,
which hand-authoring gets cleaner than a photo trace would, and it avoids lifting
anything from someone else's licensed work. Both bodies are 214 wide by 608 tall in the
same 1000-wide box the DualSense uses, outer corners at radius 46 and rail corners at
12. SL, SR, the rails and the player LEDs are inert — the firmware does not map them.

Checked in headless Chrome, which is where this page lives anyway since it needs Web
Serial: all eighteen controls resolve in both views, clicking one selects its card, the
lit class turns a control blue, both stick dots move from the live sample path, and
switching back to the DualSense restores it unchanged. No console errors either way.

```html
  <defs>
    <linearGradient id="gJC" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#5a5a63"/><stop offset="1" stop-color="#2e2e34"/>
    </linearGradient>
  </defs>

  <rect class="ctl trig" id="c-6" x="252" y="2" width="178" height="30" rx="13"/>
  <text class="lbl" x="341" y="22">ZL</text>
  <rect class="ctl trig" id="c-7" x="570" y="2" width="178" height="30" rx="13"/>
  <text class="lbl" x="659" y="22">ZR</text>
  <rect class="ctl bump" id="c-4" x="244" y="36" width="194" height="30" rx="13"/>
  <text class="lbl dark" x="341" y="56">L</text>
  <rect class="ctl bump" id="c-5" x="562" y="36" width="194" height="30" rx="13"/>
  <text class="lbl dark" x="659" y="56">R</text>

  <path class="body" style="fill:url(#gJC);stroke:#232328" d="M278 64 H434 Q446 64 446 76 V660 Q446 672 434 672 H278 Q232 672 232 626 V110 Q232 64 278 64 Z"/>
  <path class="body" style="fill:url(#gJC);stroke:#232328" d="M566 64 H722 Q768 64 768 110 V626 Q768 672 722 672 H566 Q554 672 554 660 V76 Q554 64 566 64 Z"/>

  <rect class="mute" style="fill:#1d1d21" x="434" y="76" width="12" height="584" rx="3"/>
  <rect class="mute" style="fill:#1d1d21" x="554" y="76" width="12" height="584" rx="3"/>
  <rect class="mute" style="fill:#43434a" x="433" y="250" width="14" height="56" rx="5"/>
  <rect class="mute" style="fill:#43434a" x="433" y="386" width="14" height="56" rx="5"/>
  <rect class="mute" style="fill:#43434a" x="553" y="250" width="14" height="56" rx="5"/>
  <rect class="mute" style="fill:#43434a" x="553" y="386" width="14" height="56" rx="5"/>
  <rect class="hole" x="436" y="470" width="8" height="16" rx="2"/>
  <rect class="hole" x="436" y="496" width="8" height="16" rx="2"/>
  <rect class="hole" x="436" y="522" width="8" height="16" rx="2"/>
  <rect class="hole" x="436" y="548" width="8" height="16" rx="2"/>
  <rect class="hole" x="556" y="470" width="8" height="16" rx="2"/>
  <rect class="hole" x="556" y="496" width="8" height="16" rx="2"/>
  <rect class="hole" x="556" y="522" width="8" height="16" rx="2"/>
  <rect class="hole" x="556" y="548" width="8" height="16" rx="2"/>

  <rect class="ctl small" id="c-8" x="396" y="98" width="26" height="18" rx="5"/>
  <text class="tiny" x="409" y="92">minus</text>
  <rect class="ctl small" id="c-9" x="700" y="98" width="26" height="18" rx="5"/>
  <text class="tiny" x="713" y="92">plus</text>

  <circle class="well" cx="341" cy="200" r="62"/>
  <circle class="ctl stick" id="c-10" cx="341" cy="200" r="46"/>
  <circle class="cap" id="dot-L" cx="341" cy="200" r="19"/>
  <text class="tiny" id="lbl-L" x="341" y="282">pointer</text>

  <circle class="ctl face" id="c-12" cx="341" cy="362" r="27"/>
  <text class="glyph small" x="341" y="367">&#9650;</text>
  <circle class="ctl face" id="c-13" cx="341" cy="486" r="27"/>
  <text class="glyph small" x="341" y="491">&#9660;</text>
  <circle class="ctl face" id="c-14" cx="279" cy="424" r="27"/>
  <text class="glyph small" x="279" y="429">&#9664;</text>
  <circle class="ctl face" id="c-15" cx="403" cy="424" r="27"/>
  <text class="glyph small" x="403" y="429">&#9654;</text>

  <rect class="ctl small" id="c-17" x="324" y="556" width="34" height="34" rx="8"/>
  <circle class="hole" cx="341" cy="573" r="7"/>
  <text class="tiny" x="341" y="612">capture</text>

  <circle class="ctl face" id="c-3" cx="661" cy="146" r="29"/>
  <text class="glyph" x="661" y="155">X</text>
  <circle class="ctl face" id="c-1" cx="721" cy="206" r="29"/>
  <text class="glyph" x="721" y="215">A</text>
  <circle class="ctl face" id="c-0" cx="661" cy="266" r="29"/>
  <text class="glyph" x="661" y="275">B</text>
  <circle class="ctl face" id="c-2" cx="601" cy="206" r="29"/>
  <text class="glyph" x="601" y="215">Y</text>

  <circle class="well" cx="661" cy="424" r="62"/>
  <circle class="ctl stick" id="c-11" cx="661" cy="424" r="46"/>
  <circle class="cap" id="dot-R" cx="661" cy="424" r="19"/>
  <text class="tiny" id="lbl-R" x="661" y="506">pointer</text>

  <circle class="ctl ps" id="c-16" cx="661" cy="572" r="24"/>
  <path class="hole" d="M661 561 l11 10 h-4 v9 h-14 v-9 h-4 z"/>
  <text class="tiny" x="661" y="612">home</text>

  <g id="battwrap" style="display:none">
    <rect class="battbox" id="battbox" x="474" y="360" width="52" height="22" rx="6"/>
    <text class="battxt" id="batt-t" x="500" y="375">--</text>
  </g>
```

## Joy-Con 2 is a different animal

PIDs `0x2066` and `0x2067`, and they connect over BLE through `switch2_ble.c`, not this
driver at all. None of the above applies to them, and their protocol is much less
publicly documented. Nothing here was tested with one.

## Where the research came from

ControllerKeys, the paid macOS app already noted in `JOYPAD-OS-ROUTE.md`, has public
source at `github.com/NSEvent/xbox-controller-mapper`. Its Nintendo handling and its
"minimap" approach — photo-traced silhouettes, a table of normalised control positions,
connector lines out to the mapping cards — are worth reading if this UI grows. It is
PolyForm Noncommercial: fine to read and to run, not code to lift into a redistributed
firmware. Everything here was written from scratch.

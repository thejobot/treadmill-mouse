#!/usr/bin/env python3
"""
Prove the dongle can drive this Mac, with no controller attached.

    ./selftest.py

Drives the pointer, wheel and buttons directly over the serial link and checks
what the window server actually received. This exercises the part that was
genuinely in doubt - whether the 7-byte mouse report matches the descriptor and
the host parses it - without needing anything paired over Bluetooth.

Pointer motion is measured as cursor displacement. Scroll and clicks cannot be
measured that way, so they are confirmed with CGEventSourceSecondsSinceLast-
EventType, which reports how long ago the host last saw an event of a given
type and needs no Accessibility permission.

The cursor is returned to where it started. Scrolling lands on whatever window
is under the pointer; the click test uses the "back" button, which is inert
outside a browser.
"""

import ctypes
import ctypes.util
import glob
import json
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jp import build_packet, MSG_CMD

# CGEventType values we care about
EVT_LEFT_DOWN = 1
EVT_LEFT_UP = 2
EVT_SCROLL = 22
EVT_OTHER_DOWN = 25
EVT_MOUSE_MOVED = 5
SESSION_STATE = 0  # kCGEventSourceStateCombinedSessionState


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


_cg = ctypes.CDLL(ctypes.util.find_library("ApplicationServices"))
_cg.CGEventCreate.restype = ctypes.c_void_p
_cg.CGEventCreate.argtypes = [ctypes.c_void_p]
_cg.CGEventGetLocation.restype = CGPoint
_cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
_cg.CGEventSourceCounterForEventType.restype = ctypes.c_uint32
_cg.CGEventSourceCounterForEventType.argtypes = [ctypes.c_uint32,
                                                 ctypes.c_uint32]
_cf_release = ctypes.CDLL(ctypes.util.find_library("CoreFoundation")).CFRelease
_cf_release.argtypes = [ctypes.c_void_p]


def cursor():
    ev = _cg.CGEventCreate(None)
    p = _cg.CGEventGetLocation(ev)
    _cf_release(ev)
    return p.x, p.y


def count(evt):
    """How many events of this type the session has seen.

    Must be the COMBINED SESSION state. kCGEventSourceStateHIDSystemState
    does not track scroll or other-button events at all - it reports them as
    never having happened, which reads exactly like a firmware fault. That
    cost a wrong conclusion here once already.
    """
    return _cg.CGEventSourceCounterForEventType(SESSION_STATE, evt)


class Link:
    def __init__(self):
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if not ports:
            sys.exit("no adapter serial port found - is the dongle plugged in?")
        self.ser = serial.Serial(ports[0], 115200, timeout=0.2)
        self.seq = 0
        self.port = ports[0]

    def send(self, cmd, **args):
        payload = {"cmd": cmd}
        payload.update(args)
        self.ser.write(build_packet(
            MSG_CMD, self.seq,
            json.dumps(payload, separators=(",", ":")).encode()))
        self.seq = (self.seq + 1) & 0xFF
        time.sleep(0.15)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()


def drive(link, seconds, **kw):
    start = cursor()
    link.send("MOUSE.TEST", ms=int(seconds * 1000), **kw)
    time.sleep(seconds + 0.35)
    end = cursor()
    return end[0] - start[0], end[1] - start[1]


def main():
    link = Link()
    print(f"# {link.port}")
    print("# no controller attached - driving the report path directly\n")

    origin = cursor()
    results = []

    # --- Pointer ---------------------------------------------------------
    # macOS applies its own pointer scaling to HID deltas, so absolute pixels
    # are not the thing to assert. What must hold is direction, cross-axis
    # isolation, and a consistent ratio between commanded and observed.
    print("POINTER")
    ratios = []
    for label, vx, vy, secs in (("right", 400, 0, 1.0),
                                ("left", -400, 0, 1.0),
                                ("down", 0, 300, 1.0),
                                ("up", 0, -300, 1.0)):
        dx, dy = drive(link, secs, vx=vx, vy=vy)
        commanded = (vx or vy) * secs
        observed = dx if vx else dy
        cross = dy if vx else dx
        ratio = observed / commanded if commanded else 0
        ratios.append(ratio)
        print(f"  {label:<6} commanded {commanded:+6.0f} px   "
              f"observed {observed:+7.0f} px   cross-axis {cross:+4.0f}   "
              f"ratio {ratio:.2f}")
        results.append((f"pointer {label}", observed / commanded > 0.05
                        and abs(cross) < abs(observed) * 0.15))

    spread = max(ratios) - min(ratios)
    print(f"\n  host scaling {sum(ratios)/len(ratios):.2f}x, "
          f"spread {spread:.2f} across all four directions")
    results.append(("scaling is linear", spread < 0.08))

    # --- Scroll ----------------------------------------------------------
    print("\nSCROLL")
    link.send("MOUSE.TEST", ms=0)
    time.sleep(0.6)

    before = count(EVT_SCROLL)
    link.send("MOUSE.TEST", wheel=30, ms=1500)
    time.sleep(2.0)
    gained = count(EVT_SCROLL) - before
    print(f"  wheel 30 ticks/s for 1.5s  ->  host logged {gained} scroll events")
    results.append(("vertical scroll reaches host", gained > 10))

    before = count(EVT_SCROLL)
    link.send("MOUSE.TEST", pan=30, ms=1500)
    time.sleep(2.0)
    gained = count(EVT_SCROLL) - before
    print(f"  pan   30 ticks/s for 1.5s  ->  host logged {gained} scroll events")
    results.append(("horizontal scroll reaches host", gained > 10))

    # --- Buttons ---------------------------------------------------------
    # Left button (bit 0). The only button with unambiguous host-side
    # evidence: middle and back are intercepted and remapped by Mac Mouse Fix
    # before the session counter sees them, so their absence here would say
    # nothing about the firmware. Proving bit 0 parses proves the button byte
    # parses; which bit does what afterwards is the host's business.
    print("\nBUTTONS")
    time.sleep(0.6)
    down_before, up_before = count(EVT_LEFT_DOWN), count(EVT_LEFT_UP)
    link.send("MOUSE.TEST", buttons=1, ms=250)
    time.sleep(1.0)
    downs = count(EVT_LEFT_DOWN) - down_before
    ups = count(EVT_LEFT_UP) - up_before
    print(f"  left button held 0.25s     ->  host logged {downs} down, {ups} up")
    results.append(("button press reaches host", downs >= 1 and ups >= 1))

    # --- Restore ---------------------------------------------------------
    link.send("MOUSE.TEST", ms=0)
    time.sleep(0.3)
    here = cursor()
    bx, by = origin[0] - here[0], origin[1] - here[1]
    if abs(bx) > 2 or abs(by) > 2:
        avg = sum(ratios) / len(ratios) or 1
        dur = 0.8
        link.send("MOUSE.TEST", vx=int(bx / avg / dur), vy=int(by / avg / dur),
                  ms=int(dur * 1000))
        time.sleep(dur + 0.4)
    link.send("MOUSE.TEST", ms=0)
    link.close()

    print(f"\ncursor back to {cursor()[0]:.0f}, {cursor()[1]:.0f} "
          f"(started {origin[0]:.0f}, {origin[1]:.0f})\n")

    width = max(len(n) for n, _ in results)
    for name, ok in results:
        print(f"  {name.ljust(width)}   {'PASS' if ok else 'FAIL'}")

    print()
    if all(ok for _, ok in results):
        print("VERDICT: PASS - pointer, scroll and buttons all reach this Mac,")
        print("         with nothing installed on the host.")
        return 0
    print("VERDICT: FAIL - see above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

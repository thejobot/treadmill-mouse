#!/usr/bin/env python3
"""
End-to-end test of the real DualSense driving this Mac.

    ./controller_test.py

Three phases, each prompted. Cursor movement is correlated against stick
deflection read from the adapter's own stream, so a hand on the trackpad
cannot be mistaken for the dongle working. Scroll and clicks are counted
from the window server.

Keep hands off the trackpad throughout.
"""

import ctypes
import ctypes.util
import glob
import json
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jp import build_packet, parse_packet, CDC_SYNC, MSG_CMD

SESSION = 0
EVT_LEFT_DOWN, EVT_LEFT_UP, EVT_SCROLL = 1, 2, 22

_cg = ctypes.CDLL(ctypes.util.find_library("ApplicationServices"))


class P(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


_cg.CGEventCreate.restype = ctypes.c_void_p
_cg.CGEventCreate.argtypes = [ctypes.c_void_p]
_cg.CGEventGetLocation.restype = P
_cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
_cg.CGEventSourceCounterForEventType.restype = ctypes.c_uint32
_cg.CGEventSourceCounterForEventType.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
_rel = ctypes.CDLL(ctypes.util.find_library("CoreFoundation")).CFRelease
_rel.argtypes = [ctypes.c_void_p]


def cursor():
    e = _cg.CGEventCreate(None)
    p = _cg.CGEventGetLocation(e)
    _rel(e)
    return p.x, p.y


def count(evt):
    return _cg.CGEventSourceCounterForEventType(SESSION, evt)


state = {"lx": 128, "ly": 128, "rx": 128, "ry": 128, "btn": 0, "n": 0}
stop = threading.Event()


def reader(ser):
    buf = b""
    while not stop.is_set():
        c = ser.read(512)
        if not c:
            continue
        buf += c
        while len(buf) >= 7:
            i = buf.find(bytes([CDC_SYNC]))
            if i == -1:
                buf = b""
                break
            buf = buf[i:]
            pk = parse_packet(buf)
            if pk is None:
                break
            buf = buf[pk["raw_len"]:]
            try:
                b = json.loads(pk["payload"].decode())
            except Exception:
                continue
            if isinstance(b, list) and len(b) >= 4 and b[0] == "o":
                ax = bytes.fromhex(b[3])
                if len(ax) >= 4:
                    state["lx"], state["ly"], state["rx"], state["ry"] = ax[:4]
                    state["n"] += 1
            elif isinstance(b, dict) and b.get("type") == "input":
                state["btn"] = b.get("buttons", 0)


def wait_for(check, title, prompt, timeout=90):
    """Block until the user actually does the thing, then measure.

    A fixed countdown assumes the person is watching at that instant. Waiting
    for the input itself means the test can't fail just because they looked
    away - and it can report honestly that nothing was tried.
    """
    print(f"\n{title}")
    print(f"  {prompt}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if check():
            print("  detected - measuring now")
            return True
        time.sleep(0.05)
    print(f"  nothing detected after {timeout}s")
    return False


def stick_moved(which, thresh=25):
    ax, ay = (("lx", "ly") if which == "left" else ("rx", "ry"))
    return max(abs(state[ax] - 128), abs(state[ay] - 128)) > thresh


def main():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no adapter serial port found")
    ser = serial.Serial(ports[0], 115200, timeout=0.2)
    ser.write(build_packet(MSG_CMD, 0, json.dumps(
        {"cmd": "INPUT.STREAM", "enable": True}, separators=(",", ":")).encode()))
    threading.Thread(target=reader, args=(ser,), daemon=True).start()
    time.sleep(0.6)

    if state["n"] == 0:
        stop.set()
        ser.close()
        sys.exit("no controller data - is the DualSense connected?")

    results = []

    # --- 1. Pointer -----------------------------------------------------
    started = wait_for(lambda: stick_moved("right"), "PHASE 1 - POINTER",
                       "Move the RIGHT stick. Keep going for ~15s once it starts. "
                       "Do not touch the trackpad.")
    if not started:
        stop.set()
        ser.close()
        sys.exit("right stick never moved - nothing was tested")
    active = idle = 0.0
    n_act = n_idle = 0
    peak = 0
    last = cursor()
    t = time.time() + 15
    while time.time() < t:
        time.sleep(0.02)
        n = cursor()
        d = ((n[0] - last[0]) ** 2 + (n[1] - last[1]) ** 2) ** 0.5
        last = n
        defl = max(abs(state["rx"] - 128), abs(state["ry"] - 128))
        peak = max(peak, defl)
        if defl > 12:
            active += d
            n_act += 1
        else:
            idle += d
            n_idle += 1
    print(f"  peak deflection {peak} counts")
    print(f"  cursor travel while pushed : {active:8.0f} px  ({n_act} samples)")
    print(f"  cursor travel while centred: {idle:8.0f} px  ({n_idle} samples)")
    results.append(("right stick moves cursor", active > 500 and peak > 40))
    results.append(("cursor still when centred", idle < max(40, active * 0.03)))

    # --- 2. Scroll ------------------------------------------------------
    if wait_for(lambda: stick_moved("left"), "PHASE 2 - SCROLL",
                "Push the LEFT stick up and down. Keep going for ~10s."):
        before = count(EVT_SCROLL)
        peak_l = 0
        t = time.time() + 10
        while time.time() < t:
            time.sleep(0.02)
            peak_l = max(peak_l,
                         max(abs(state["lx"] - 128), abs(state["ly"] - 128)))
        gained = count(EVT_SCROLL) - before
        print(f"  peak left-stick deflection {peak_l} counts")
        print(f"  host logged {gained} scroll events")
        results.append(("left stick scrolls", gained > 10 and peak_l > 40))
    else:
        results.append(("left stick scrolls", False))

    # --- 3. Click -------------------------------------------------------
    # Wait for R2 itself: JP_BUTTON_R2 is bit 7.
    if wait_for(lambda: state["btn"] & (1 << 7), "PHASE 3 - CLICK",
                "Press and release R2 (right trigger) a few times."):
        d_before, u_before = count(EVT_LEFT_DOWN), count(EVT_LEFT_UP)
        t = time.time() + 8
        while time.time() < t:
            time.sleep(0.05)
        downs = count(EVT_LEFT_DOWN) - d_before
        ups = count(EVT_LEFT_UP) - u_before
        print(f"  host logged {downs} left-button down, {ups} up")
        results.append(("R2 is left click", downs >= 1 and ups >= 1))
    else:
        results.append(("R2 is left click", False))

    stop.set()
    ser.write(build_packet(MSG_CMD, 1, json.dumps(
        {"cmd": "INPUT.STREAM", "enable": False}, separators=(",", ":")).encode()))
    time.sleep(0.3)
    ser.close()

    print(f"\n{state['n']} controller samples received\n")
    w = max(len(n) for n, _ in results)
    for name, ok in results:
        print(f"  {name.ljust(w)}   {'PASS' if ok else 'FAIL'}")
    print()
    if all(ok for _, ok in results):
        print("VERDICT: PASS - the DualSense drives this Mac as a mouse.")
        return 0
    print("VERDICT: FAIL - see above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Passive end-to-end recorder for the real DualSense.

    ./record_test.py [seconds]

Records for a fixed window and works out afterwards what was exercised, so it
never needs to prompt mid-run. Do, in any order and at any pace:

  1. move the RIGHT stick   -> pointer
  2. move the LEFT stick    -> scroll
  3. press R2               -> left click

Cursor movement is attributed only to samples where the right stick was
actually deflected, so a hand on the trackpad cannot be mistaken for the
dongle working. Keep off the trackpad anyway.
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
JP_R2 = 1 << 7

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


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no adapter serial port found")

    ser = serial.Serial(ports[0], 115200, timeout=0.2)
    ser.write(build_packet(MSG_CMD, 0, json.dumps(
        {"cmd": "INPUT.STREAM", "enable": True}, separators=(",", ":")).encode()))
    threading.Thread(target=reader, args=(ser,), daemon=True).start()
    time.sleep(0.6)

    scroll0 = count(EVT_SCROLL)
    down0, up0 = count(EVT_LEFT_DOWN), count(EVT_LEFT_UP)

    travel_active = travel_idle = 0.0
    n_act = n_idle = 0
    peak_r = peak_l = 0
    r2_seen = False
    scroll_during_left = 0
    left_active_samples = 0

    # Live guidance, so someone running this in their own terminal is told
    # what to do while it happens rather than after.
    third = secs / 3.0
    banners = [
        (0.0, "NOW: move the RIGHT stick in circles  (pointer)"),
        (third, "NOW: push the LEFT stick up and down  (scroll)"),
        (third * 2, "NOW: press and release R2 a few times  (click)"),
    ]
    next_banner = 0

    print("Keep your hands OFF the trackpad for the whole run.\n", flush=True)

    last = cursor()
    prev_scroll = scroll0
    t_start = time.time()
    t_end = t_start + secs
    while time.time() < t_end:
        elapsed = time.time() - t_start
        if next_banner < len(banners) and elapsed >= banners[next_banner][0]:
            print(f"[{elapsed:5.1f}s] {banners[next_banner][1]}", flush=True)
            next_banner += 1
        time.sleep(0.02)
        now = cursor()
        d = ((now[0] - last[0]) ** 2 + (now[1] - last[1]) ** 2) ** 0.5
        last = now

        rd = max(abs(state["rx"] - 128), abs(state["ry"] - 128))
        ld = max(abs(state["lx"] - 128), abs(state["ly"] - 128))
        peak_r = max(peak_r, rd)
        peak_l = max(peak_l, ld)
        if state["btn"] & JP_R2:
            r2_seen = True

        if rd > 12:
            travel_active += d
            n_act += 1
        elif ld <= 12:
            # Only call it "idle" when neither stick is engaged, so scrolling
            # with the left stick is not counted as unexplained cursor drift.
            travel_idle += d
            n_idle += 1

        cur_scroll = count(EVT_SCROLL)
        if ld > 12:
            scroll_during_left += cur_scroll - prev_scroll
            left_active_samples += 1
        prev_scroll = cur_scroll

    stop.set()
    ser.write(build_packet(MSG_CMD, 1, json.dumps(
        {"cmd": "INPUT.STREAM", "enable": False}, separators=(",", ":")).encode()))
    time.sleep(0.3)
    ser.close()

    downs = count(EVT_LEFT_DOWN) - down0
    ups = count(EVT_LEFT_UP) - up0

    print(f"controller samples      : {state['n']}")
    print(f"peak right-stick        : {peak_r} counts")
    print(f"peak left-stick         : {peak_l} counts")
    print(f"R2 seen pressed         : {'yes' if r2_seen else 'no'}")
    print()
    print(f"cursor travel, R stick pushed : {travel_active:8.0f} px ({n_act} samples)")
    print(f"cursor travel, both centred   : {travel_idle:8.0f} px ({n_idle} samples)")
    print(f"scroll events while L pushed  : {scroll_during_left:8d}    ({left_active_samples} samples)")
    print(f"left-button events            : {downs} down, {ups} up")
    print()

    results = []
    if peak_r > 40:
        results.append(("right stick moves cursor", travel_active > 400))
        results.append(("no drift when centred",
                        travel_idle < max(30, travel_active * 0.05)))
    else:
        results.append(("right stick moves cursor", None))
    if peak_l > 40:
        results.append(("left stick scrolls", scroll_during_left > 10))
    else:
        results.append(("left stick scrolls", None))
    if r2_seen:
        results.append(("R2 is left click", downs >= 1 and ups >= 1))
    else:
        results.append(("R2 is left click", None))

    w = max(len(n) for n, _ in results)
    for name, ok in results:
        tag = "NOT TRIED" if ok is None else ("PASS" if ok else "FAIL")
        print(f"  {name.ljust(w)}   {tag}")
    print()

    tried = [r for _, r in results if r is not None]
    if not tried:
        print("VERDICT: nothing was exercised.")
        return 1
    if all(tried) and len(tried) == len(results):
        print("VERDICT: PASS - the DualSense drives this Mac as a mouse.")
        return 0
    if all(tried):
        print("VERDICT: everything tried passed; the rest was not exercised.")
        return 1
    print("VERDICT: FAIL - see above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

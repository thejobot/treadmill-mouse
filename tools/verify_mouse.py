#!/usr/bin/env python3
"""
Prove the dongle is what moves the cursor.

    ./verify_mouse.py [seconds]

Samples two things at once and correlates them:

  1. Right-stick deflection, read from the adapter's own input stream.
  2. Cursor position, read from the window server.

A previous session claimed success from cursor travel alone, which cannot
distinguish the dongle from the hand on the trackpad. This reports movement
only in the windows where the stick was actually deflected, and separately
reports movement while the stick was centred, which should be ~0.

Do not touch the real mouse or trackpad while this runs.
"""

import ctypes
import ctypes.util
import glob
import json
import sys
import threading
import time

import serial

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from jp import build_packet, parse_packet, CDC_SYNC, MSG_CMD

# --- cursor position via CoreGraphics -------------------------------------


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


_cg = ctypes.CDLL(ctypes.util.find_library("ApplicationServices"))
_cg.CGEventCreate.restype = ctypes.c_void_p
_cg.CGEventCreate.argtypes = [ctypes.c_void_p]
_cg.CGEventGetLocation.restype = CGPoint
_cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
_cf_release = ctypes.CDLL(ctypes.util.find_library("CoreFoundation")).CFRelease
_cf_release.argtypes = [ctypes.c_void_p]


def cursor():
    ev = _cg.CGEventCreate(None)
    p = _cg.CGEventGetLocation(ev)
    _cf_release(ev)
    return p.x, p.y


# --- stick state from the adapter -----------------------------------------

# Compact stream events look like ['o', player, addr, '<hex axes>'] where the
# hex is LX LY RX RY L2 R2 RZ. 'o' is post-profile output, which is what the
# mouse conversion actually consumes.
state = {"rx": 128, "ry": 128, "lx": 128, "ly": 128, "seen": 0}
stop = threading.Event()


def reader(ser):
    buf = b""
    while not stop.is_set():
        chunk = ser.read(256)
        if not chunk:
            continue
        buf += chunk
        while len(buf) >= 7:
            sync = buf.find(bytes([CDC_SYNC]))
            if sync == -1:
                buf = b""
                break
            buf = buf[sync:]
            pkt = parse_packet(buf)
            if pkt is None:
                break
            buf = buf[pkt["raw_len"]:]
            try:
                body = json.loads(pkt["payload"].decode())
            except Exception:
                continue
            axes = None
            if isinstance(body, list) and len(body) >= 4 and body[0] == "o":
                axes = bytes.fromhex(body[3])
            elif isinstance(body, dict) and body.get("type") == "input":
                axes = bytes(body.get("axes", []))
            if axes and len(axes) >= 4:
                state["lx"], state["ly"], state["rx"], state["ry"] = axes[:4]
                state["seen"] += 1


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no adapter serial port found")

    ser = serial.Serial(ports[0], 115200, timeout=0.2)
    ser.write(build_packet(MSG_CMD, 0,
                           json.dumps({"cmd": "INPUT.STREAM", "enable": True},
                                      separators=(",", ":")).encode()))
    t = threading.Thread(target=reader, args=(ser,), daemon=True)
    t.start()

    print(f"# {ports[0]}  {seconds:.0f}s  -- move the RIGHT STICK, do not touch the mouse")

    # Deflection beyond this counts as "the stick is being pushed". Wide enough
    # to ignore both rest offset and noise.
    THRESHOLD = 12

    travel_active = 0.0     # cursor movement while stick deflected
    travel_idle = 0.0       # cursor movement while stick centred
    samples_active = 0
    samples_idle = 0
    max_defl = 0

    last = cursor()
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(0.02)
        now = cursor()
        d = ((now[0] - last[0]) ** 2 + (now[1] - last[1]) ** 2) ** 0.5
        last = now

        dx = state["rx"] - 128
        dy = state["ry"] - 128
        defl = max(abs(dx), abs(dy))
        max_defl = max(max_defl, defl)

        if defl > THRESHOLD:
            travel_active += d
            samples_active += 1
        else:
            travel_idle += d
            samples_idle += 1

    stop.set()
    ser.write(build_packet(MSG_CMD, 1,
                           json.dumps({"cmd": "INPUT.STREAM", "enable": False},
                                      separators=(",", ":")).encode()))
    time.sleep(0.3)
    ser.close()

    print()
    print(f"controller samples received : {state['seen']}")
    print(f"peak right-stick deflection : {max_defl} counts")
    print(f"cursor travel WHILE PUSHED  : {travel_active:8.0f} px over {samples_active} samples")
    print(f"cursor travel WHILE CENTRED : {travel_idle:8.0f} px over {samples_idle} samples")
    print()

    if state["seen"] == 0:
        print("VERDICT: no controller data at all - the DualSense is not connected.")
    elif max_defl <= THRESHOLD:
        print("VERDICT: stick never deflected - inconclusive, nothing was tested.")
    elif travel_active > 50 and travel_idle < 5:
        print("VERDICT: PASS - cursor moves when and only when the stick is pushed.")
    elif travel_active > 50:
        print("VERDICT: cursor moved while pushed, but also while centred.")
        print("         Either the mouse was touched, or the pointer is drifting.")
    else:
        print("VERDICT: FAIL - stick deflected but the cursor did not move.")


if __name__ == "__main__":
    main()

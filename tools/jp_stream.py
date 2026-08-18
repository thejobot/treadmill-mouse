#!/usr/bin/env python3
"""
Watch live controller input from a joypad-os adapter.

    ./jp_stream.py [seconds]

Enables INPUT.STREAM and prints every event packet that arrives, so you can
see whether the controller is actually reaching the adapter and what the
sticks are reporting.
"""

import glob
import json
import sys
import time

import serial

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from jp import build_packet, parse_packet, CDC_SYNC, MSG_CMD, TYPE_NAMES


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no adapter serial port found")
    ser = serial.Serial(ports[0], 115200, timeout=0.2)
    print(f"# {ports[0]}  streaming {seconds:.0f}s")

    blob = json.dumps({"cmd": "INPUT.STREAM", "enable": True},
                      separators=(",", ":")).encode()
    ser.write(build_packet(MSG_CMD, 0, blob))

    buf = b""
    seen = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = ser.read(512)
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
            seen += 1
            name = TYPE_NAMES.get(pkt["type"], hex(pkt["type"]))
            try:
                body = json.loads(pkt["payload"].decode())
            except Exception:
                body = pkt["payload"].hex()
            print(f"[{name}] {body}")

    blob = json.dumps({"cmd": "INPUT.STREAM", "enable": False},
                      separators=(",", ":")).encode()
    ser.write(build_packet(MSG_CMD, 1, blob))
    ser.close()
    print(f"# {seen} packets")


if __name__ == "__main__":
    main()

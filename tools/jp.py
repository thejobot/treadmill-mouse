#!/usr/bin/env python3
"""
Talk to a joypad-os adapter over its CDC serial port, one shot at a time.

The upstream cdc_test.py is an interactive REPL. This is the same wire
protocol driven from the command line, so it can be scripted.

    ./jp.py INFO
    ./jp.py MODE.LIST
    ./jp.py BT.STATUS PLAYERS.LIST
    ./jp.py MODE.SET mode=10

Port is found automatically; override with --port.

Needs pyserial: pip install pyserial

The serial port is present in Keyboard/Mouse mode as well as SInput, in
both the composite identity and the mouse-only one, so tuning stays live
while you are feeling the result. It is the one thing the mouse-only
identity keeps besides the mouse itself.
"""

import glob
import json
import struct
import sys
import time

import serial

CDC_SYNC = 0xAA
MSG_CMD = 0x01
TYPE_NAMES = {0x02: "RSP", 0x03: "EVT", 0x04: "ACK", 0x05: "NAK", 0x10: "DAT"}


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def build_packet(msg_type: int, seq: int, payload: bytes) -> bytes:
    header = struct.pack("<BHBB", CDC_SYNC, len(payload), msg_type, seq)
    crc = crc16_ccitt(struct.pack("BB", msg_type, seq) + payload)
    return header + payload + struct.pack("<H", crc)


def parse_packet(data: bytes):
    if len(data) < 7 or data[0] != CDC_SYNC:
        return None
    length = struct.unpack("<H", data[1:3])[0]
    if len(data) < 7 + length:
        return None
    msg_type, seq = data[3], data[4]
    payload = data[5 : 5 + length]
    crc_rx = struct.unpack("<H", data[5 + length : 7 + length])[0]
    if crc_rx != crc16_ccitt(struct.pack("BB", msg_type, seq) + payload):
        return None
    return {"type": msg_type, "seq": seq, "payload": payload, "raw_len": 7 + length}


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("No /dev/cu.usbmodem* found. Is the adapter plugged in and in SInput mode?")
    return ports[0]


def parse_args(tokens):
    """Split ['MODE.SET', 'mode=10'] into ('MODE.SET', {'mode': 10})."""
    cmd, args = tokens[0].upper(), {}
    for tok in tokens[1:]:
        key, _, val = tok.partition("=")
        try:
            args[key] = int(val)
        except ValueError:
            args[key] = val
    return cmd, args


def drain(ser, seconds: float):
    """Read for a fixed window and print every complete packet."""
    buf = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
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
            buf = buf[pkt["raw_len"] :]
            name = TYPE_NAMES.get(pkt["type"], f"0x{pkt['type']:02x}")
            try:
                body = json.dumps(json.loads(pkt["payload"].decode()), indent=2)
            except Exception:
                body = pkt["payload"].hex()
            print(f"<< [{name}] {body}")


def main():
    argv = sys.argv[1:]
    port = None
    if "--port" in argv:
        i = argv.index("--port")
        port = argv[i + 1]
        del argv[i : i + 2]
    if not argv:
        sys.exit(__doc__)

    port = port or find_port()
    print(f"# {port}")
    ser = serial.Serial(port, 115200, timeout=0.2)

    seq = 0
    # Commands are separated by bare tokens that contain a dot or are all-caps
    # words; simplest rule that works: each argv entry with no '=' starts a new
    # command, and following 'k=v' entries belong to it.
    groups = []
    for tok in argv:
        if "=" in tok and groups:
            groups[-1].append(tok)
        else:
            groups.append([tok])

    for tokens in groups:
        cmd, args = parse_args(tokens)
        payload = {"cmd": cmd}
        payload.update(args)
        blob = json.dumps(payload, separators=(",", ":")).encode()
        print(f">> {cmd} {args if args else ''}".rstrip())
        ser.write(build_packet(MSG_CMD, seq, blob))
        seq = (seq + 1) & 0xFF
        drain(ser, 0.6)

    ser.close()


if __name__ == "__main__":
    main()

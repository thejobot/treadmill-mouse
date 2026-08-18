#!/usr/bin/env python3
"""
Tune the DualSense mouse. Changes apply instantly; `save` makes them stick.

    ./mouse.py                       show current settings
    ./mouse.py scroll 25             vertical scroll ticks/sec
    ./mouse.py scroll 25 18          vertical and horizontal
    ./mouse.py speed 2400            pointer pixels/sec at full deflection
    ./mouse.py natural on            invert vertical scroll (macOS style)
    ./mouse.py hscroll off           suppress sideways scrolling entirely
    ./mouse.py swap on               pointer on left stick, scroll on right
    ./mouse.py curve 40 75           pointer curve, scroll curve (0-100)
    ./mouse.py precision 30          speed % while L3 held
    ./mouse.py turbo 250             speed % while R3 held
    ./mouse.py save                  persist to the dongle's flash
    ./mouse.py reset                 back to defaults (then `save`)
    ./mouse.py cal                   relearn stick centre - sticks released

Settings live on the dongle, so they follow it to any Mac. Nothing is
installed on the host and no permissions are needed.

Curve is 0-100: lower is closer to cubic, which gives finer control near
centre; higher is closer to square, which feels more immediate. Scroll
defaults higher than the pointer because macOS has no pixel-precise path for
a generic HID wheel, so every tick is a whole line and a near-cubic curve
just makes slow scrolling feel dead.
"""

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_venv = HERE.parent / ".venv" / "bin" / "python"
PY = _venv if _venv.exists() else Path(sys.executable)
JP = HERE / "jp.py"

FIELDS = ["speed", "scroll_v", "scroll_h", "curve", "scroll_curve",
          "precision", "turbo", "noise", "invert_v", "invert_h",
          "swap", "no_hscroll"]


def call(*args):
    out = subprocess.run([str(PY), str(JP), *args],
                         capture_output=True, text=True).stdout
    return out


def show():
    out = call("MOUSE.GET")
    vals = {}
    for line in out.splitlines():
        line = line.strip().rstrip(",")
        if '":' in line:
            k, _, v = line.partition('":')
            vals[k.strip().lstrip('"')] = v.strip()
    if not vals:
        print("No response. Is the dongle plugged in?")
        print(out)
        return
    print(f"  pointer speed   {vals.get('speed')} px/sec at full deflection")
    print(f"  scroll          {vals.get('scroll_v')} vertical, "
          f"{vals.get('scroll_h')} horizontal ticks/sec")
    print(f"  curve           {vals.get('curve')} pointer, "
          f"{vals.get('scroll_curve')} scroll")
    print(f"  precision (L3)  {vals.get('precision')}%")
    print(f"  turbo (R3)      {vals.get('turbo')}%")
    print(f"  natural scroll  {'on' if vals.get('invert_v') == '1' else 'off'}")
    print(f"  horizontal      {'off' if vals.get('no_hscroll') == '1' else 'on'}")
    print(f"  sticks swapped  {'yes' if vals.get('swap') == '1' else 'no'}")
    print(f"  noise floor     {vals.get('noise')} counts")


def onoff(word):
    if word in ("on", "yes", "1", "true"):
        return 1
    if word in ("off", "no", "0", "false"):
        return 0
    sys.exit(f"expected on or off, got {word!r}")


def main():
    if len(sys.argv) < 2:
        show()
        return

    cmd = sys.argv[1].lower()
    rest = sys.argv[2:]

    if cmd == "save":
        print("saved" if '"ok": true' in call("MOUSE.SAVE") else "save failed")
        return
    if cmd == "reset":
        call("MOUSE.RESET")
        show()
        print("\n(run `./mouse.py save` to keep these)")
        return
    if cmd == "cal":
        print("recalibrated" if '"ok": true' in call("MOUSE.CAL") else "failed")
        return
    if cmd in ("show", "get"):
        show()
        return

    args = []
    if cmd == "scroll":
        if not rest:
            sys.exit("usage: mouse.py scroll <vertical> [horizontal]")
        args.append(f"scroll_v={int(rest[0])}")
        if len(rest) > 1:
            args.append(f"scroll_h={int(rest[1])}")
    elif cmd == "speed":
        args.append(f"speed={int(rest[0])}")
    elif cmd == "curve":
        args.append(f"curve={int(rest[0])}")
        if len(rest) > 1:
            args.append(f"scroll_curve={int(rest[1])}")
    elif cmd == "precision":
        args.append(f"precision={int(rest[0])}")
    elif cmd == "turbo":
        args.append(f"turbo={int(rest[0])}")
    elif cmd == "noise":
        args.append(f"noise={int(rest[0])}")
    elif cmd == "natural":
        args.append(f"invert_v={onoff(rest[0])}")
    elif cmd == "hscroll":
        # "hscroll off" means suppress it, which is the no_hscroll flag set.
        args.append(f"no_hscroll={1 - onoff(rest[0])}")
    elif cmd == "swap":
        args.append(f"swap={onoff(rest[0])}")
    else:
        sys.exit(__doc__)

    call("MOUSE.SET", *args)
    show()
    print("\n(run `./mouse.py save` to keep these)")


if __name__ == "__main__":
    main()

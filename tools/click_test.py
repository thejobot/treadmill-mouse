#!/usr/bin/env python3
"""
Prove R2 on the controller is a left click.

    ./click_test.py [seconds]

Counts left-button events the window server receives. Press ONLY R2 and keep
off the trackpad - then every click counted came from the dongle.

The stream's compact packets carry axes but not buttons, and the verbose ones
arrive too rarely to sample, so button state is not read back from the
adapter. Isolation is what makes this conclusive instead: nothing else is
touched, so nothing else can be producing clicks.
"""

import ctypes
import ctypes.util
import sys
import time

_cg = ctypes.CDLL(ctypes.util.find_library("ApplicationServices"))
_cg.CGEventSourceCounterForEventType.restype = ctypes.c_uint32
_cg.CGEventSourceCounterForEventType.argtypes = [ctypes.c_uint32, ctypes.c_uint32]

SESSION = 0
DOWN, UP = 1, 2


def count(evt):
    return _cg.CGEventSourceCounterForEventType(SESSION, evt)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    print(f"Press R2 (right trigger) about five times over the next {secs:.0f}s.")
    print("Do not touch the trackpad.\n", flush=True)

    d0, u0 = count(DOWN), count(UP)
    end = time.time() + secs
    while time.time() < end:
        left = end - time.time()
        print(f"  {left:4.1f}s remaining   clicks so far: "
              f"{count(DOWN) - d0}", end="\r", flush=True)
        time.sleep(0.1)

    downs, ups = count(DOWN) - d0, count(UP) - u0
    print(" " * 50, end="\r")
    print(f"left-button down: {downs}")
    print(f"left-button up  : {ups}\n")

    if downs >= 1 and downs == ups:
        print("VERDICT: PASS - R2 is a left click.")
        return 0
    if downs >= 1:
        print(f"VERDICT: PASS with a caveat - {downs} down vs {ups} up.")
        return 0
    print("VERDICT: no clicks seen. If you pressed R2, that is a real failure.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

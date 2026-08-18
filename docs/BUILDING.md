# Building it

You do not have to build anything to use this. The
[latest release](../../../releases/latest) has the `.uf2`. This is for changing
it.

## The overlay

`overlay/` holds the sixteen files this project changes or adds, at the exact
paths joypad-os uses. Thirteen are modifications, three are new. That is the
whole of the difference between upstream and this firmware.

```
./setup.sh                 # clones into ./build-tree
./setup.sh ~/somewhere     # or wherever you want it
```

That clones joypad-os, checks out the pinned commit, pulls its submodules and
copies the overlay over the top. Afterwards:

```
git -C build-tree diff              # every modification, in context
git -C build-tree status            # the three added files show as untracked
```

Re-running `setup.sh` is safe. It hard-resets the checkout to the pinned commit
before copying, so a half-finished attempt cannot leave stale files behind.

The pin is `0273263`, which is joypad-os 2.2.0 plus a few commits. Moving it
forward is a matter of changing `UPSTREAM_REF`, re-running, and reading the
conflicts by hand — there is no patch to fail, since the overlay is whole files.

## The toolchain

You need `arm-none-eabi-gcc` and `cmake`.

Get the compiler from
[Arm's own downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads),
as a tarball, unpacked wherever you like. It needs no admin rights and no
installer. On macOS in particular, avoid `brew install arm-none-eabi-gcc`: it
pulls binutils from a third-party tap rather than from Arm.

## Building

```
cd build-tree/src
mkdir -p build && cd build

export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"
cmake .. -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Release
cmake --build . --target joypad_bt2usb -j8
```

Name the target. Without it, cmake builds every app in the tree and stops on an
unrelated one.

The output is `build/joypad_bt2usb.uf2`.

Do not use the repository's `Makefile` on macOS. It looks for the toolchain in
`/Applications/ArmGNUToolchain` and gives up if it is not there, whatever is on
your `PATH`.

## Flashing

Triple-click the button on the board. It reappears as a drive called RP2350.

Copy the file with a tool that verifies, not by dragging it in a file manager:

```
picotool load build/joypad_bt2usb.uf2
picotool reboot
```

Dragging works most of the time and fails silently the rest. A truncated image
starts, faults and resets in a loop, which looks exactly like a fast-blinking
light and no USB device — and looks nothing like a copy that did not finish.
Two builds in this project's history were diagnosed as code bugs before that
turned out to be the cause. `picotool` writes directly and verifies every byte.

If the board is not enumerating at all, hold BOOTSEL while plugging it in. That
path is in the boot ROM and cannot be broken by anything you flash, so the board
cannot be bricked.

## After flashing

Reflashing clears the Bluetooth bond. Re-pair: on a DualSense, hold CREATE and
PS together for about five seconds until the light bar double-blinks.

Bump `FW_REV` in `src/fw_rev.h` when you build something you intend to keep. It
is deliberately hand-edited. CMake's own `BUILD_TIME` is stamped at configure
time, not build time, so it sits still through ordinary rebuilds and has
already caused one wrong answer to "did the flash take".

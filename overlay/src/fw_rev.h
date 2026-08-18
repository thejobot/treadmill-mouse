// fw_rev.h - which build of the pure-mouse work is on the board
// SPDX-License-Identifier: Apache-2.0
//
// INFO's "build" field cannot answer this. CMake stamps BUILD_TIME with
// string(TIMESTAMP) at *configure* time, so it only moves when cmake
// reconfigures - an ordinary rebuild leaves it reading whatever it read weeks
// ago. That has already caused one wrong reading of "did the flash take".
//
// This is bumped by hand, deliberately, and matches the .uf2 filename in
// firmware/. Naming the release is what is actually wanted when asking what is
// on the board, and a hand-bumped string cannot silently go stale the way an
// automatic one did: if it is wrong, it is wrong loudly and at the same moment
// the file is named.

#ifndef FW_REV_H
#define FW_REV_H

#define FW_REV "puremouse-v20"

#endif // FW_REV_H

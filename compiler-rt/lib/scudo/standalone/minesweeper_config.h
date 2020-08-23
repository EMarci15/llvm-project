//===-- minesweeper_config.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_MINESWEEPER_CONFIG_H_
#define SCUDO_MINESWEEPER_CONFIG_H_

#include "internal_defs.h"

namespace scudo {
  constexpr bool IgnoreDecommitSize = true;
  constexpr uptr SweepThreshold
                   = /* Sweep when */25/* % of all allocated memory is in quarantine...*/;
  constexpr uptr DecommitThreshold
                   = /* ...or decommitted memory size */300/* % of allocated memory. */;
  constexpr bool IgnoreFailedFreeSize = true;
  constexpr uptr SmallGranuleSizeLog = 5; // 32B
  constexpr bool SingleThread = false;
  constexpr uptr BackStopThreshold = 5;
  constexpr uptr MB = 1024*1024;
  constexpr uptr BackStopLeeway = 100*MB;
  constexpr uptr ImmediateReleaseMinSize = 4096 * 4;
  constexpr bool MinesweeperUnmapping = true;
  constexpr bool MinesweeperZeroing = true;
}

#endif // SCUDO_MINESWEEPER_CONFIG_H_

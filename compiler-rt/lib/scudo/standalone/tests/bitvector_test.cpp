//===-- bitvector_test.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "tests/scudo_unit_test.h"

#include "bitvector.h"
#include "internal_defs.h"

using uptr = scudo::uptr;

TEST(ScudoBitvectorTest, SetGet) {
  scudo::BitVector v;
  v.init(4*4096*8); // 4 pages

  const uptr Index = 40000;
  EXPECT_FALSE(v[Index]);

  v.set(Index);
  EXPECT_TRUE(v[Index]);
  EXPECT_FALSE(v[Index+1]);
  EXPECT_FALSE(v[Index+8]);
  EXPECT_FALSE(v[Index+64]);
  EXPECT_TRUE(v[Index]);

  v.clear(Index);
  EXPECT_FALSE(v[Index]);
  EXPECT_FALSE(v[Index+1]);
}

TEST(ScudoBitvectorTest, RangeClear) {
  scudo::BitVector v;
  v.init(4*4096*8); // 4 pages

  const uptr StartIndex = 40000;
  const uptr StartClearIndex = 40075;
  const uptr EndClearIndex = 40920;
  const uptr EndIndex = 41000;

  for (uptr Index = StartIndex; Index < EndIndex; Index++) {
    EXPECT_FALSE(v[Index]);
    v.set(Index);
  }

  EXPECT_FALSE(v[StartIndex-1]);
  for (uptr Index = StartIndex; Index < EndIndex; Index++) {
    EXPECT_TRUE(v[Index]);
  }
  EXPECT_FALSE(v[EndIndex]);

  v.clear(StartClearIndex, EndClearIndex);
  
  EXPECT_FALSE(v[StartIndex-1]);
  for (uptr Index = StartIndex; Index < StartClearIndex; Index++) {
    EXPECT_TRUE(v[Index]);
  }
  for (uptr Index = StartClearIndex; Index <= EndClearIndex; Index++) {
    EXPECT_FALSE(v[Index]);
  }
  for (uptr Index = EndClearIndex+1; Index < EndIndex; Index++) {
    EXPECT_TRUE(v[Index]);
  }
  EXPECT_FALSE(v[EndIndex]);
}

TEST(ScudoBitvectorTest, AllZero) {
  scudo::BitVector v;
  v.init(4*4096*8); // 4 pages

  const uptr StartIndex = 40000;
  const uptr StartClearIndex = 40075;
  const uptr EndClearIndex = 40920;
  const uptr EndIndex = 41000;

  for (uptr Index = StartIndex; Index <= EndIndex; Index++) {
    EXPECT_FALSE(v[Index]);
    v.set(Index);
  }

  v.clear(StartClearIndex, EndClearIndex);

  // Start
  EXPECT_TRUE(v.allZero(StartIndex-10, StartIndex-10));
  EXPECT_TRUE(v.allZero(StartIndex-10, StartIndex-1));
  EXPECT_FALSE(v.allZero(StartIndex-10, StartIndex));

  // End
  EXPECT_TRUE(v.allZero(EndIndex+10, EndIndex+10));
  EXPECT_TRUE(v.allZero(EndIndex+1, EndIndex+10));
  EXPECT_FALSE(v.allZero(EndIndex, EndIndex+10));

  // Clear
  EXPECT_TRUE(v.allZero(StartClearIndex, EndClearIndex));
  EXPECT_FALSE(v.allZero(StartClearIndex-1, EndClearIndex));
  EXPECT_FALSE(v.allZero(StartClearIndex, EndClearIndex+1));

  // Long
  EXPECT_FALSE(v.allZero(StartIndex-128, EndIndex+128));

  // Short
  EXPECT_FALSE(v.allZero(StartIndex+10, StartIndex+10));
  EXPECT_FALSE(v.allZero(StartIndex, StartIndex));
  EXPECT_TRUE(v.allZero(StartIndex-1, StartIndex-1));
  EXPECT_TRUE(v.allZero(StartIndex-100, StartIndex-100));
}

TEST(ScudoBitvectorTest, Large) {
  scudo::BitVector v;
  v.init(100000ull*4096*8); // 100000 pages

  const uptr StartIndex = 100000;
  const uptr StartClearIndex = 10100;
  const uptr EndClearIndex = 199900;
  const uptr EndIndex = 200000;
  const uptr Stride = 100;

  for (uptr Index = StartIndex; Index <= EndIndex; Index+=Stride) {
    EXPECT_FALSE(v[Index]);
    v.set(Index);
    EXPECT_TRUE(v[Index]);
  }

  v.clear(StartClearIndex, EndClearIndex);
  
  for (uptr Index = StartIndex; Index <= EndIndex; Index+=Stride) {
    if ((Index < StartClearIndex) || (Index > EndClearIndex)) {
      EXPECT_TRUE(v[Index]);
    } else {
      EXPECT_FALSE(v[Index]);
    }
  }
}

TEST(SudoShadowBitMapTest, FractionalBlocks) {
  const uptr BS = 32;

  scudo::ShadowBitMap bm;
  bm.init(0, 1ull<<40, BS);

  const uptr BlockAddr = BS*10000;
  bm.set(BlockAddr+BS/4);

  // operator[]
  EXPECT_FALSE(bm[BlockAddr-1]);
  EXPECT_TRUE(bm[BlockAddr]);
  EXPECT_TRUE(bm[BlockAddr+BS/2]);
  EXPECT_FALSE(bm[BlockAddr+BS]);

  // allZero()
  EXPECT_TRUE(bm.allZero(BlockAddr-2*BS, BlockAddr-3*BS/2));
  EXPECT_TRUE(bm.allZero(BlockAddr-BS, BlockAddr));
  EXPECT_FALSE(bm.allZero(BlockAddr-BS, BlockAddr+2*BS));
  EXPECT_FALSE(bm.allZero(BlockAddr, BlockAddr+1));
  EXPECT_FALSE(bm.allZero(BlockAddr-500*BS, BlockAddr+500*BS));
  EXPECT_FALSE(bm.allZero(BlockAddr+BS-1, BlockAddr+BS));
  EXPECT_FALSE(bm.allZero(BlockAddr+BS-1, BlockAddr+BS+1));
  EXPECT_TRUE(bm.allZero(BlockAddr+BS, BlockAddr+2*BS));

  // clear()
  bm.clear(BlockAddr+BS/4, BlockAddr+BS/2);
  EXPECT_TRUE(bm.allZero(BlockAddr-BS, BlockAddr+2*BS));
}


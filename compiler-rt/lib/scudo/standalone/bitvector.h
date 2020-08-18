//===-- bitvector.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_BITVECTOR_H_
#define SCUDO_BITVECTOR_H_

#include "common.h"
#include "mutex.h"

namespace scudo {

// Flat array of bits backed by pages on-demand.
class BitVector {
private:
  inline uptr ActualMapSizeBytes() {
    uptr RequiredMapSizeBytes = divRoundUp(Size,BITS_PER_ENTRY) * sizeof(mapT);
    return roundUpTo(RequiredMapSizeBytes, 4096);
  }

public:
  using mapT = u64;
  const static uptr BITS_PER_ENTRY = sizeof(mapT)*8;
  const uptr ENTRIES_PER_PAGE = getPageSizeSlow()/sizeof(mapT);

  void mapArray(uptr Size) {
    this->Size = Size;

    Map = (mapT*)map(NULL, ActualMapSizeBytes(), "BitVector", MAP_ONDEMAND);
  }

  void init(uptr Size) { mapArray(Size); }

  void set(uptr Index) {
    DCHECK_LT(Index, Size);
    arrayEntry(Index) |= subMask(Index);
  }

  void clear(uptr Index) {
    DCHECK_LT(Index, Size);
    arrayEntry(Index) |= ~subMask(Index);
  }

  void clear() {
    releasePagesToOS((uptr)Map, 0, ActualMapSizeBytes());
  }

  void clear(uptr From, uptr To) {
    const mapT startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    const mapT endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    const uptr startIndex = arrIndex(From);
    const uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      mapT mask = startMask & endMask; // single entry -- 1s from From to To
      Map[startIndex] |= ~mask;
      return;
    } else {
      // Zero start and end
      Map[startIndex] &= ~startMask;
      Map[endIndex] &= ~endMask;

      // Zero partial pages at start and end
      const uptr firstFullPage = roundUpTo(startIndex, ENTRIES_PER_PAGE);
      const uptr lastFullPage = roundDownTo(endIndex+1, ENTRIES_PER_PAGE);
      for (uptr index = startIndex+1; (index < firstFullPage) && (index < endIndex); index++) {
        Map[index] = 0;
      }
      if (firstFullPage <= lastFullPage) {
        for (uptr index = lastFullPage; index < endIndex; index++) {
          Map[index] = 0;
        }
      }

      // Unmap full pages in the middle
      uptr releaseStart = (uptr)&Map[firstFullPage];
      uptr releaseSize = lastFullPage - firstFullPage;
      releasePagesToOS(releaseStart, 0, releaseSize * sizeof(mapT));
    }
  }

  bool operator[](uptr Index) {
    DCHECK_LT(Index, Size);
    return arrayEntry(Index) & subMask(Index);
  }

  bool allZero(uptr From, uptr To) {
    mapT startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    mapT endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    uptr startIndex = arrIndex(From);
    uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      mapT mask = startMask & endMask; // single entry -- 1s from From to To
      return (Map[startIndex] & mask)==0;
    } else {
      // Check start and end
      bool startNotAllZero = Map[startIndex] & startMask;
      bool endNotAllZero = Map[endIndex] & endMask;
      if (startNotAllZero || endNotAllZero) return false;

      // Check middle
      for (uptr index = startIndex+1; index < endIndex; index++) {
        if (Map[index]) return false;
      }
      return true;
    }
  }

  void disable() {}
  void enable() {}

private:
  uptr Size;
  mapT *Map;

  inline uptr arrIndex(uptr Index) const { return Index / BITS_PER_ENTRY; }
  inline uptr subIndex(uptr Index) const { return Index % BITS_PER_ENTRY; }

  inline mapT& arrayEntry(uptr Index) const { return Map[arrIndex(Index)]; }
  inline mapT subMask(uptr Index) const { return ((mapT)1) << subIndex(Index); }
};

// A class recording a boolean for each block of memory (of size BlockSize),
// in the address range [Start,Start+MemSize)
class ShadowBitMap : private BitVector {
public:
  void init(uptr Start, uptr MemSize, uptr BlockSizeLog) {
    this->Start = Start;
    this->MemSize = MemSize;
    this->BlockSizeLog = BlockSizeLog;
    BitVector::init(divRoundUp(MemSize, ((uptr)1)<<BlockSizeLog));
 }

  void set(uptr Ptr) {
    dcheck_valid(Ptr);
    BitVector::set(index(Ptr));
  }

  void clear(uptr Ptr) {
    dcheck_valid(Ptr);
    BitVector::clear(index(Ptr));
  }

  void clear() {
    BitVector::clear();
  }

  void clear(uptr From, uptr To) {
    BitVector::clear(index(From), index(To));
  }

  bool operator[](uptr Ptr) {
    dcheck_valid(Ptr);
    return BitVector::operator[](index(Ptr));
  }

  bool allZero(uptr From, uptr To) {
    dcheck_valid(From);
    dcheck_valid(To);
    To = roundUpTo(To, ((uptr)1) << BlockSizeLog);
    return BitVector::allZero(index(From), index(To)-1);
  }

  void disable() {}
  void enable() {}

private:
  uptr Start, MemSize, BlockSizeLog;

  uptr index(uptr ptr) {
    return (ptr - Start) >> BlockSizeLog;
  }

  inline void dcheck_valid(uptr Ptr) {
    DCHECK_GE(Ptr, Start);
    DCHECK_LT(Ptr, Start+MemSize);
  }
};


} // namespace scudo

#endif // SCUDO_BITVECTOR_H_

//===-- bitvector.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_BITVECTOR_H_
#define SCUDO_BITVECTOR_H_

#include "atomic_helpers.h"
#include "common.h"
#include "mutex.h"

namespace scudo {

// Flat array of bits backed by pages on-demand.
class BitVector {
public:
  using mapT = atomic_u64;
  using mapValT = mapT::Type;
  const static uptr BITS_PER_ENTRY = sizeof(mapT)*8;
  const uptr ENTRIES_PER_PAGE = getPageSizeSlow()/sizeof(mapT);

  void mapArray(uptr Size) {
    uptr RequiredMapSizeBytes = divRoundUp(Size,BITS_PER_ENTRY) * sizeof(mapT);
    uptr ActualMapSizeBytes = roundUpTo(RequiredMapSizeBytes, 4096);
    
    this->Size = Size;
    Map = (mapT*)map(NULL, ActualMapSizeBytes, "BitVector", MAP_ONDEMAND);
    MapSize = ActualMapSizeBytes / sizeof(mapT); // Calculate number of entries
  }

  void init(uptr Size) { mapArray(Size); }

  void set(uptr Index) {
    DCHECK_LT(Index, Size);
    atomic_or_fetch(arrayEntry(Index), subMask(Index));
  }

  void clear(uptr Index) {
    DCHECK_LT(Index, Size);
    atomic_and_fetch(arrayEntry(Index), ~subMask(Index));
  }

  void clear() {
    releasePagesToOS((uptr)Map, 0, MapSize);
  }

  void clear(uptr From, uptr To) {
    const mapValT startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    const mapValT endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    const uptr startIndex = arrIndex(From);
    const uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      mapValT mask = startMask & endMask; // single entry -- 1s from From to To
      atomic_and_fetch(&Map[startIndex], ~mask);
      return;
    } else {
      // Zero start and end
      atomic_and_fetch(&Map[startIndex], ~startMask);
      atomic_and_fetch(&Map[endIndex], ~endMask);

      // Zero partial pages at start and end
      const uptr firstFullPage = roundUpTo(startIndex+1, ENTRIES_PER_PAGE);
      const uptr lastFullPage = roundDownTo(endIndex, ENTRIES_PER_PAGE);
      for (uptr index = startIndex+1; (index < firstFullPage) && (index < endIndex); index++) {
        atomic_store_relaxed(&Map[index], 0);
      }
      if (firstFullPage <= lastFullPage) {
        for (uptr index = lastFullPage; index < endIndex; index++) {
          atomic_store_relaxed(&Map[index], 0);
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
    return atomic_load_relaxed(arrayEntry(Index)) & subMask(Index);
  }

  bool allZero(uptr From, uptr To) {
    mapValT startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    mapValT endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    uptr startIndex = arrIndex(From);
    uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      mapValT mask = startMask & endMask; // single entry -- 1s from From to To
      return (atomic_load_relaxed(&Map[startIndex]) & mask)==0;
    } else {
      // Check start and end
      bool startNotAllZero = atomic_load_relaxed(&Map[startIndex]) & startMask;
      bool endNotAllZero = atomic_load_relaxed(&Map[endIndex]) & endMask;
      if (startNotAllZero || endNotAllZero) return false;

      // Check middle
      for (uptr index = startIndex+1; index < endIndex; index++) {
        if (atomic_load_relaxed(&Map[index])) return false;
      }
      return true;
    }
  }

  void disable() {}
  void enable() {}

private:
  uptr Size, MapSize;
  mapT *Map;

  inline uptr arrIndex(uptr Index) { return Index / BITS_PER_ENTRY; }
  inline uptr subIndex(uptr Index) { return Index % BITS_PER_ENTRY; }

  inline mapT* arrayEntry(uptr Index) { return &Map[arrIndex(Index)]; }
  inline mapValT subMask(uptr Index) { return ((mapValT)1) << subIndex(Index); }
};

// A class recording a boolean for each block of memory (of size BlockSize),
// in the address range [Start,Start+MemSize)
class ShadowBitMap : private BitVector {
public:
  void init(uptr Start, uptr MemSize, uptr BlockSize) {
    this->Start = Start;
    this->MemSize = MemSize;
    this->BlockSize = BlockSize; 
    BitVector::init(divRoundUp(MemSize, BlockSize));
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
    return BitVector::allZero(index(From), index(To));
  }

  void disable() {}
  void enable() {}

private:
  uptr Start, MemSize, BlockSize;

  uptr index(uptr ptr) {
    return (ptr - Start) / BlockSize;
  }

  inline void dcheck_valid(uptr Ptr) {
    DCHECK_GE(Ptr, Start);
    DCHECK_LT(Ptr, Start+MemSize);
  }
};


} // namespace scudo

#endif // SCUDO_BITVECTOR_H_

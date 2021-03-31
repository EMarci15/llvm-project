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
template<typename mapT, typename T = mapT>
class BitVectorBase {
private:
  inline uptr ActualMapSizeBytes() {
    uptr RequiredMapSizeBytes = divRoundUp(Size,BITS_PER_ENTRY) * sizeof(mapT);
    return roundUpTo(RequiredMapSizeBytes, 4096);
  }

public:
  const static uptr BITS_PER_ENTRY = sizeof(mapT)*8;
  const uptr ENTRIES_PER_PAGE = getPageSizeSlow()/sizeof(mapT);

  void mapArray(uptr Size) {
    this->Size = Size;
    Map = (mapT*)map(NULL, ActualMapSizeBytes(), "BitVector", MAP_ONDEMAND);
  }

  void init(uptr Size) { mapArray(Size); }

  void clear() { releasePagesToOS((uptr)Map, 0, ActualMapSizeBytes()); }

  void disable() {}
  void enable() {}

protected:
  uptr Size;
  mapT *Map;

  inline uptr arrIndex(uptr Index) const { return Index / BITS_PER_ENTRY; }
  inline uptr subIndex(uptr Index) const { return Index % BITS_PER_ENTRY; }

  inline mapT& arrayEntry(uptr Index) const { return Map[arrIndex(Index)]; }
  inline T subMask(uptr Index) const { return ((T)1) << subIndex(Index); }
};

template<typename mapT>
class BitVector : public BitVectorBase<mapT> {
  using Base = BitVectorBase<mapT>;
  using Base::arrayEntry;
  using Base::subMask;
  using Base::arrIndex;
  using Base::Map;
  using Base::Size;

public:
  void set(uptr Index) {
    DCHECK_LT(Index, Size);
    arrayEntry(Index) |= subMask(Index);
  }

  using Base::clear;

  void clear(uptr Index) {
    DCHECK_LT(Index, Size);
    arrayEntry(Index) &= ~subMask(Index);
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

  void clear(uptr From, uptr To) {
    mapT startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    mapT endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    uptr startIndex = arrIndex(From);
    uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      mapT mask = ~(startMask & endMask); // single entry -- 0s from From to To
      Map[startIndex] &= mask;
    } else {
      // Check start and end
      Map[startIndex] &= ~startMask;
      Map[endIndex] &= ~endMask;

      // Check middle
      for (uptr index = startIndex+1; index < endIndex; index++)
        Map[index] = (mapT)0;
    }
  }
};

template<typename A>
class AtomicBitVector : public BitVectorBase<A, typename A::Type> {
  using Base = BitVectorBase<A, typename A::Type>;
  using Base::arrayEntry;
  using Base::subMask;
  using Base::arrIndex;
  using Base::Map;
  using Base::Size;
  using T = typename A::Type;

public:
  void set(uptr Index, memory_order mo = memory_order_relaxed) {
    DCHECK_LT(Index, Size);
    atomic_or_fetch(&arrayEntry(Index), subMask(Index), mo);
  }

  void clear(uptr Index, memory_order mo = memory_order_relaxed) {
    DCHECK_LT(Index, Size);
    atomic_and_fetch(&arrayEntry(Index), ~subMask(Index), mo);
  }

  bool get(uptr Index, memory_order mo = memory_order_relaxed) {
    DCHECK_LT(Index, Size);
    return (bool)(atomic_load(&arrayEntry(Index), mo) & subMask(Index));
  }

  void set(uptr From, uptr To, memory_order mo = memory_order_relaxed) {
    T startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    T endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    uptr startIndex = arrIndex(From);
    uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      T mask = startMask & endMask; // single entry -- 1s from From to To
      atomic_or_fetch(&Map[startIndex], mask, mo);
    } else {
      // Check start and end
      atomic_or_fetch(&Map[startIndex], startMask, mo);
      atomic_or_fetch(&Map[endIndex], endMask, mo);

      // Check middle
      for (uptr index = startIndex+1; index < endIndex; index++)
        atomic_store(&Map[index], (T)(-1), mo);
    }
  }

  void clear(uptr From, uptr To, memory_order mo = memory_order_relaxed) {
    T startMask = ~(subMask(From)-1); // 1s at & above subIndex(From)
    T endMask = ((subMask(To)<<1)-1); // 1s at & below subIndex(To)

    uptr startIndex = arrIndex(From);
    uptr endIndex = arrIndex(To);

    if (startIndex == endIndex) {
      T mask = ~(startMask & endMask); // single entry -- 0s from From to To
      atomic_and_fetch(&Map[startIndex], mask, mo);
    } else {
      // Check start and end
      atomic_and_fetch(&Map[startIndex], ~startMask, mo);
      atomic_and_fetch(&Map[endIndex], ~endMask, mo);

      // Check middle
      for (uptr index = startIndex+1; index < endIndex; index++)
        atomic_store(&Map[index], (T)(0), mo);
    }
  }
};

// A class recording a boolean for each block of memory (of size BlockSize),
// in the address range [Start,Start+MemSize)
template<typename BV>
class ShadowBitMap {
public:
  void init(uptr Start, uptr MemSize, uptr BlockSizeLog) {
    this->Start = Start;
    this->MemSize = MemSize;
    this->BlockSizeLog = BlockSizeLog;
    Map.init(divRoundUp(MemSize, ((uptr)1)<<BlockSizeLog));
  }

  void clear() {
    Map.clear();
  }

  void disable() {}
  void enable() {}
protected:
  BV Map;
  uptr Start, MemSize, BlockSizeLog;

  uptr index(uptr ptr) {
    return (ptr - Start) >> BlockSizeLog;
  }

  inline void dcheck_valid(uptr Ptr) {
    DCHECK_GE(Ptr, Start);
    DCHECK_LT(Ptr, Start+MemSize);
  }
};

class ShadowT : public ShadowBitMap<BitVector<u64>> {
public:
  void set(uptr Ptr) {
    dcheck_valid(Ptr);
    Map.set(index(Ptr));
  }

  void clear() { Map.clear(); }

  void clear(uptr Ptr) {
    dcheck_valid(Ptr);
    Map.clear(index(Ptr));
  }
  
  void clear(uptr From, uptr To) {
    dcheck_valid(From);
    dcheck_valid(To-1);
    Map.clear(index(From), index(To-1));
  }

  bool operator[](uptr Ptr) {
    dcheck_valid(Ptr);
    return Map[index(Ptr)];
  }

  bool allZero(uptr From, uptr To) {
    dcheck_valid(From);
    dcheck_valid(To);
    To = roundUpTo(To, ((uptr)1) << BlockSizeLog);
    return Map.allZero(index(From), index(To)-1);
  }
};

class AtomicShadowT : public ShadowBitMap<AtomicBitVector<atomic_u64>> {
public:
  void clear(uptr From, uptr To, memory_order mo = memory_order_relaxed) {
    dcheck_valid(From);
    dcheck_valid(To);
    To = roundUpTo(To, ((uptr)1) << BlockSizeLog);
    Map.clear(index(From), index(To)-1, mo);
  }

  void set(uptr From, uptr To, memory_order mo = memory_order_relaxed) {
    dcheck_valid(From);
    dcheck_valid(To);
    To = roundUpTo(To, ((uptr)1) << BlockSizeLog);
    Map.set(index(From), index(To)-1, mo);
  }

  bool get(uptr Ptr, memory_order mo = memory_order_relaxed) {
    dcheck_valid(Ptr);
    return Map.get(index(Ptr), mo);
  }
};

} // namespace scudo

#endif // SCUDO_BITVECTOR_H_

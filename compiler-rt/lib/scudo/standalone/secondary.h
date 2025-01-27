//===-- secondary.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_SECONDARY_H_
#define SCUDO_SECONDARY_H_

#include "common.h"
#include "list.h"
#include "mutex.h"
#include "stats.h"
#include "string_utils.h"

namespace scudo {

// This allocator wraps the platform allocation primitives, and as such is on
// the slower side and should preferably be used for larger sized allocations.
// Blocks allocated will be preceded and followed by a guard page, and hold
// their own header that is not checksummed: the guard pages and the Combined
// header should be enough for our purpose.

namespace LargeBlock {

struct Header {
  LargeBlock::Header *Prev;
  LargeBlock::Header *Next;
  uptr BlockEnd;
  uptr MapBase;
  uptr MapSize;
  MapPlatformData Data;
};

constexpr uptr getHeaderSize() {
  return roundUpTo(sizeof(Header), 1U << SCUDO_MIN_ALIGNMENT_LOG);
}

static Header *getHeader(uptr Ptr) {
  return reinterpret_cast<Header *>(Ptr - getHeaderSize());
}

static Header *getHeader(const void *Ptr) {
  return getHeader(reinterpret_cast<uptr>(Ptr));
}

struct SavedHeader {
  uptr Block;
  uptr BlockEnd;
  uptr MapBase;
  uptr MapSize;
  MapPlatformData Data;

  uptr minAddress() const { return Block; }
  uptr maxAddress() const { return BlockEnd; }
};

} // namespace LargeBlock

class MapAllocatorNoCache {
public:
  void initLinkerInitialized(UNUSED s32 ReleaseToOsInterval) {}
  void init(UNUSED s32 ReleaseToOsInterval) {}
  bool retrieve(UNUSED uptr Size, UNUSED LargeBlock::Header **H,
                UNUSED FillContentsMode FillContents) {
    return false;
  }
  bool store(UNUSED LargeBlock::Header *H) { return false; }
  bool store(const LargeBlock::SavedHeader& H) { return false; }
  static bool canCache(UNUSED uptr Size) { return false; }
  void disable() {}
  void enable() {}
  void releaseToOS() {}
  void setReleaseToOsIntervalMs(UNUSED s32 Interval) {}
};

template <uptr MaxEntriesCount = 32U, uptr MaxEntrySize = 1UL << 19,
          s32 MinReleaseToOsIntervalMs = INT32_MIN,
          s32 MaxReleaseToOsIntervalMs = INT32_MAX>
class MapAllocatorCache {
public:
  // Fuchsia doesn't allow releasing Secondary blocks yet. Note that 0 length
  // arrays are an extension for some compilers.
  // FIXME(kostyak): support (partially) the cache on Fuchsia.
  static_assert(!SCUDO_FUCHSIA || MaxEntriesCount == 0U, "");

  void initLinkerInitialized(s32 ReleaseToOsInterval) {
    setReleaseToOsIntervalMs(ReleaseToOsInterval);
  }
  void init(s32 ReleaseToOsInterval) {
    memset(this, 0, sizeof(*this));
    initLinkerInitialized(ReleaseToOsInterval);
  }

  bool store(const LargeBlock::SavedHeader& H) {
    CachedBlock Entry;
    Entry.Block = H.Block;
    Entry.BlockEnd = H.BlockEnd;
    Entry.MapBase = H.MapBase;
    Entry.MapSize = H.MapSize;
    Entry.Data = H.Data;
    Entry.Time = 0; // Already released
    Entry.NoAccess = MinesweeperUnmapping;

    return store(Entry);
  }

  bool store(LargeBlock::Header *H) {
    const u64 Time = getMonotonicTime();

    CachedBlock Entry;
    Entry.Block = reinterpret_cast<uptr>(H);
    Entry.BlockEnd = H->BlockEnd;
    Entry.MapBase = H->MapBase;
    Entry.MapSize = H->MapSize;
    Entry.Data = H->Data;
    Entry.Time = Time;
    Entry.NoAccess = false;

    return store(Entry);
  }

  bool retrieve(uptr Size, LargeBlock::Header **H, FillContentsMode FillContents) {
    bool Fill = false;
    bool Served = false;
    const uptr PageSize = getPageSizeCached();
    {
      ScopedLock L(Mutex);
      if (EntriesCount == 0)
        return false;
      for (uptr I = 0; I < MaxEntriesCount; I++) {
        if (!Entries[I].Block)
          continue;
        const uptr BlockSize = Entries[I].BlockEnd - Entries[I].Block;
        if (Size > BlockSize)
          continue;
        if (Size < BlockSize - PageSize * 4U)
          continue;
        *H = reinterpret_cast<LargeBlock::Header *>(Entries[I].Block);
        Entries[I].Block = 0;
        if (Entries[I].NoAccess) {
          map((void*)*H, Entries[I].BlockEnd - (uptr)*H, "scudo:secondary:recommit");
        } else Fill = FillContents;
        (*H)->BlockEnd = Entries[I].BlockEnd;
        (*H)->MapBase = Entries[I].MapBase;
        (*H)->MapSize = Entries[I].MapSize;
        (*H)->Data = Entries[I].Data;
        EntriesCount--;
        Served = true;
        break;
      }
    }

    if (UNLIKELY(Fill)) {
        void *Ptr = reinterpret_cast<void *>(reinterpret_cast<uptr>(*H) +
                                               LargeBlock::getHeaderSize());
        memset(Ptr, FillContents == ZeroFill ? 0 : PatternFillByte,
               (*H)->BlockEnd - reinterpret_cast<uptr>(Ptr));
    }
    return Served;
  }

  static bool canCache(uptr Size) {
    return MaxEntriesCount != 0U && Size <= MaxEntrySize;
  }

  void setReleaseToOsIntervalMs(s32 Interval) {
    if (Interval >= MaxReleaseToOsIntervalMs) {
      Interval = MaxReleaseToOsIntervalMs;
    } else if (Interval <= MinReleaseToOsIntervalMs) {
      Interval = MinReleaseToOsIntervalMs;
    }
    atomic_store(&ReleaseToOsIntervalMs, Interval, memory_order_relaxed);
  }

  void releaseToOS() { releaseOlderThan(UINT64_MAX); }

  void disable() { Mutex.lock(); }

  void enable() { Mutex.unlock(); }

private:
  void empty() {
    struct {
      void *MapBase;
      uptr MapSize;
      MapPlatformData Data;
    } MapInfo[MaxEntriesCount];
    uptr N = 0;
    {
      ScopedLock L(Mutex);
      for (uptr I = 0; I < MaxEntriesCount; I++) {
        if (!Entries[I].Block)
          continue;
        MapInfo[N].MapBase = reinterpret_cast<void *>(Entries[I].MapBase);
        MapInfo[N].MapSize = Entries[I].MapSize;
        MapInfo[N].Data = Entries[I].Data;
        Entries[I].Block = 0;
        N++;
      }
      EntriesCount = 0;
      IsFullEvents = 0;
    }
    for (uptr I = 0; I < N; I++)
      unmap(MapInfo[I].MapBase, MapInfo[I].MapSize, UNMAP_ALL,
            &MapInfo[I].Data);
  }

  void releaseOlderThan(u64 Time) {
    ScopedLock L(Mutex);
    if (!EntriesCount)
      return;
    for (uptr I = 0; I < MaxEntriesCount; I++) {
      if (!Entries[I].Block || !Entries[I].Time || Entries[I].Time > Time)
        continue;
      releasePagesToOS(Entries[I].Block, 0,
                       Entries[I].BlockEnd - Entries[I].Block,
                       &Entries[I].Data);
      Entries[I].Time = 0;
    }
  }

  s32 getReleaseToOsIntervalMs() {
    return atomic_load(&ReleaseToOsIntervalMs, memory_order_relaxed);
  }

  struct CachedBlock {
    uptr Block;
    uptr BlockEnd;
    uptr MapBase;
    uptr MapSize;
    MapPlatformData Data;
    bool NoAccess;
    u64 Time;
  };
  
  inline bool store(const CachedBlock& Entry) {
    const u64 Time = getMonotonicTime();
    bool EntryCached = false;
    bool EmptyCache = false;
    {
      ScopedLock L(Mutex);
      if (EntriesCount == MaxEntriesCount) {
        if (IsFullEvents++ == 4U)
          EmptyCache = true;
      } else {
        for (uptr I = 0; I < MaxEntriesCount; I++) {
          if (Entries[I].Block)
            continue;
          if (I != 0)
            Entries[I] = Entries[0];
          Entries[0] = Entry;
          EntriesCount++;
          EntryCached = true;
          break;
        }
      }
    }
    s32 Interval;
    if (EmptyCache)
      empty();
    else if ((Interval = getReleaseToOsIntervalMs()) >= 0)
      releaseOlderThan(Time - static_cast<u64>(Interval) * 1000000);
    return EntryCached;
  }

  HybridMutex Mutex;
  CachedBlock Entries[MaxEntriesCount];
  u32 EntriesCount;
  uptr LargestSize;
  u32 IsFullEvents;
  atomic_s32 ReleaseToOsIntervalMs;
};

template <class CacheT> class MapAllocator {
public:
  void initLinkerInitialized(GlobalStats *S, s32 ReleaseToOsInterval = -1) {
    Cache.initLinkerInitialized(ReleaseToOsInterval);
    Stats.initLinkerInitialized();
    if (LIKELY(S))
      S->link(&Stats);
  }
  void init(GlobalStats *S, s32 ReleaseToOsInterval = -1) {
    memset(this, 0, sizeof(*this));
    initLinkerInitialized(S, ReleaseToOsInterval);
  }

  void *allocate(uptr Size, uptr AlignmentHint = 0, uptr *BlockEnd = nullptr,
                 FillContentsMode FillContents = NoFill);

  void recycle(const LargeBlock::SavedHeader& Header);
  void deallocate(void *Ptr);
  LargeBlock::SavedHeader decommit(void *Ptr);

  static uptr getBlockEnd(void *Ptr) {
    return LargeBlock::getHeader(Ptr)->BlockEnd;
  }

  static uptr getBlockSize(void *Ptr) {
    return getBlockEnd(Ptr) - reinterpret_cast<uptr>(Ptr);
  }

  void getStats(ScopedString *Str) const;

  void disable() {
    IterMutex.lock();
    Mutex.lock();
    Cache.disable();
  }

  void enable() {
    Cache.enable();
    Mutex.unlock();
    IterMutex.unlock();
  }

  template <typename F> void iterateOverBlocks(F Callback) {
    ScopedLock L(IterMutex);
    LargeBlock::Header *H;
    {
      ScopedLock L(Mutex);
      H = InUseBlocks.front();
      IterBlock = H;
    }
    while (H) {
      Callback(reinterpret_cast<uptr>(H) + LargeBlock::getHeaderSize());

      {
        ScopedLock L(Mutex);
        H = H->Next;
        IterBlock = H;
      }
    }
  }

  static uptr canCache(uptr Size) { return CacheT::canCache(Size); }

  void setReleaseToOsIntervalMs(s32 Interval) {
    Cache.setReleaseToOsIntervalMs(Interval);
  }

  void releaseToOS() { Cache.releaseToOS(); }

  uptr getTotalAllocatedUser() { return AllocatedBytes - FreedBytes; }

private:
  CacheT Cache;

  HybridMutex Mutex;
  DoublyLinkedList<LargeBlock::Header> InUseBlocks;
  uptr AllocatedBytes;
  uptr FreedBytes;
  uptr LargestSize;
  u32 NumberOfAllocs;
  u32 NumberOfFrees;
  LocalStats Stats;

  HybridMutex IterMutex;
  LargeBlock::Header* IterBlock;

  // Lock Mutex, and ensure that the scan (iterateOverBlocks()) is not currently in H.
  // If so, we cannot remove H now, as that would disrupt the scan.
  inline void lockOn(LargeBlock::Header* H);
  void lockOnSlow(LargeBlock::Header* H);
};

// As with the Primary, the size passed to this function includes any desired
// alignment, so that the frontend can align the user allocation. The hint
// parameter allows us to unmap spurious memory when dealing with larger
// (greater than a page) alignments on 32-bit platforms.
// Due to the sparsity of address space available on those platforms, requesting
// an allocation from the Secondary with a large alignment would end up wasting
// VA space (even though we are not committing the whole thing), hence the need
// to trim off some of the reserved space.
// For allocations requested with an alignment greater than or equal to a page,
// the committed memory will amount to something close to Size - AlignmentHint
// (pending rounding and headers).
template <class CacheT>
void *MapAllocator<CacheT>::allocate(uptr Size, uptr AlignmentHint,
                                     uptr *BlockEnd,
                                     FillContentsMode FillContents) {
  DCHECK_GE(Size, AlignmentHint);
  const uptr PageSize = getPageSizeCached();
  const uptr RoundedSize =
      roundUpTo(Size + LargeBlock::getHeaderSize(), PageSize);

  if (AlignmentHint < PageSize && CacheT::canCache(RoundedSize)) {
    LargeBlock::Header *H;
    if (Cache.retrieve(RoundedSize, &H, FillContents)) {
      if (BlockEnd)
        *BlockEnd = H->BlockEnd;
      void *Ptr = reinterpret_cast<void *>(reinterpret_cast<uptr>(H) +
                                           LargeBlock::getHeaderSize());
      const uptr BlockSize = H->BlockEnd - reinterpret_cast<uptr>(H);
      {
        ScopedLock L(Mutex);
        InUseBlocks.push_back(H);
        AllocatedBytes += BlockSize;
        NumberOfAllocs++;
        Stats.add(StatAllocated, BlockSize);
        Stats.add(StatMapped, H->MapSize);
      }
      return Ptr;
    }
  }

  MapPlatformData Data = {};
  const uptr MapSize = RoundedSize + 2 * PageSize;
  uptr MapBase =
      reinterpret_cast<uptr>(map(nullptr, MapSize, "scudo:secondary",
                                 MAP_NOACCESS | MAP_ALLOWNOMEM, &Data));
  if (UNLIKELY(!MapBase))
    return nullptr;
  uptr CommitBase = MapBase + PageSize;
  uptr MapEnd = MapBase + MapSize;

  // In the unlikely event of alignments larger than a page, adjust the amount
  // of memory we want to commit, and trim the extra memory.
  if (UNLIKELY(AlignmentHint >= PageSize)) {
    // For alignments greater than or equal to a page, the user pointer (eg: the
    // pointer that is returned by the C or C++ allocation APIs) ends up on a
    // page boundary , and our headers will live in the preceding page.
    CommitBase = roundUpTo(MapBase + PageSize + 1, AlignmentHint) - PageSize;
    const uptr NewMapBase = CommitBase - PageSize;
    DCHECK_GE(NewMapBase, MapBase);
    // We only trim the extra memory on 32-bit platforms: 64-bit platforms
    // are less constrained memory wise, and that saves us two syscalls.
    if (SCUDO_WORDSIZE == 32U && NewMapBase != MapBase) {
      unmap(reinterpret_cast<void *>(MapBase), NewMapBase - MapBase, 0, &Data);
      MapBase = NewMapBase;
    }
    const uptr NewMapEnd = CommitBase + PageSize +
                           roundUpTo((Size - AlignmentHint), PageSize) +
                           PageSize;
    DCHECK_LE(NewMapEnd, MapEnd);
    if (SCUDO_WORDSIZE == 32U && NewMapEnd != MapEnd) {
      unmap(reinterpret_cast<void *>(NewMapEnd), MapEnd - NewMapEnd, 0, &Data);
      MapEnd = NewMapEnd;
    }
  }

  const uptr CommitSize = MapEnd - PageSize - CommitBase;
  const uptr Ptr =
      reinterpret_cast<uptr>(map(reinterpret_cast<void *>(CommitBase),
                                 CommitSize, "scudo:secondary", 0, &Data));
  LargeBlock::Header *H = reinterpret_cast<LargeBlock::Header *>(Ptr);
  H->MapBase = MapBase;
  H->MapSize = MapEnd - MapBase;
  H->BlockEnd = CommitBase + CommitSize;
  H->Data = Data;
  if (BlockEnd)
    *BlockEnd = CommitBase + CommitSize;
  {
    ScopedLock L(Mutex);
    InUseBlocks.push_back(H);
    AllocatedBytes += CommitSize;
    if (LargestSize < CommitSize)
      LargestSize = CommitSize;
    NumberOfAllocs++;
    Stats.add(StatAllocated, CommitSize);
    Stats.add(StatMapped, H->MapSize);
  }
  return reinterpret_cast<void *>(Ptr + LargeBlock::getHeaderSize());
}

template <class CacheT> void MapAllocator<CacheT>::deallocate(void *Ptr) {
  LargeBlock::Header *H = LargeBlock::getHeader(Ptr);
  const uptr Block = reinterpret_cast<uptr>(H);
  const uptr CommitSize = H->BlockEnd - Block;

  lockOn(H);

  InUseBlocks.remove(H);
  FreedBytes += CommitSize;
  NumberOfFrees++;
  Stats.sub(StatAllocated, CommitSize);
  Stats.sub(StatMapped, H->MapSize);

  Mutex.unlock();

  if (CacheT::canCache(CommitSize) && Cache.store(H))
    return;
  void *Addr = reinterpret_cast<void *>(H->MapBase);
  const uptr Size = H->MapSize;
  MapPlatformData Data = H->Data;
  unmap(Addr, Size, UNMAP_ALL, &Data);
}

template <class CacheT> void MapAllocator<CacheT>::recycle(const LargeBlock::SavedHeader& Header) {
  if (CacheT::canCache(Header.BlockEnd - Header.Block)) {
    if (Cache.store(Header))
      return;
  }
  void *Addr = reinterpret_cast<void *>(Header.MapBase);
  const uptr Size = Header.MapSize;
  MapPlatformData Data = Header.Data;
  unmap(Addr, Size, UNMAP_ALL, &Data);
}

template <class CacheT> LargeBlock::SavedHeader MapAllocator<CacheT>::decommit(void *Ptr) {
  LargeBlock::Header *H = LargeBlock::getHeader(Ptr);
  const uptr Block = reinterpret_cast<uptr>(H);
  const uptr CommitSize = H->BlockEnd - Block;

  lockOn(H);

  InUseBlocks.remove(H);
  FreedBytes += CommitSize;
  NumberOfFrees++;
  Stats.sub(StatAllocated, CommitSize);
  Stats.sub(StatMapped, H->MapSize);

  Mutex.unlock();

  LargeBlock::SavedHeader Save;
  Save.Block = Block;
  Save.BlockEnd = H->BlockEnd;
  Save.MapBase = H->MapBase;
  Save.MapSize = H->MapSize;
  Save.Data = H->Data;

  MapPlatformData Data = H->Data;
  if (MinesweeperUnmapping) {
    map((void*)Block, Save.BlockEnd - Block, "scudo:secondary:decommit", MAP_NOACCESS, &Data);
  } else if (MinesweeperZeroing) {
    memset((void*)Block, 0, Save.BlockEnd - Block);
  }
  return Save;
}

template <class CacheT>
inline void MapAllocator<CacheT>::lockOn(LargeBlock::Header* H) {
  Mutex.lock();
  if (IterBlock != H) return; // success

  Mutex.unlock();
  lockOnSlow(H);
}

template <class CacheT>
void MapAllocator<CacheT>::lockOnSlow(LargeBlock::Header* H) {
  const u8 NumTries = 3U;
  const u8 NumSpinTries = 8U;
  const u8 NumYields = 8U;

  for (u8 Tries = 0; Tries < NumTries; Tries++) {
    for (u8 t = 0; t < NumSpinTries; t++) {
      yieldProcessor(NumYields);
      if (IterBlock != H) break;
    }

    Mutex.lock();
    if (IterBlock != H) return; // Locked and checked

    // Failure, we must unlock and wait
    Mutex.unlock();
    yieldProcessor(NumYields);
  }

  // Failed to quickly lock, wait for iteration to finish with secondary and lock
  {
    ScopedLock L(IterMutex);
    Mutex.lock();
    DCHECK(IterBlock == NULL);
  }
}

template <class CacheT>
void MapAllocator<CacheT>::getStats(ScopedString *Str) const {
  Str->append(
      "Stats: MapAllocator: allocated %zu times (%zuK), freed %zu times "
      "(%zuK), remains %zu (%zuK) max %zuM\n",
      NumberOfAllocs, AllocatedBytes >> 10, NumberOfFrees, FreedBytes >> 10,
      NumberOfAllocs - NumberOfFrees, (AllocatedBytes - FreedBytes) >> 10,
      LargestSize >> 20);
}

} // namespace scudo

#endif // SCUDO_SECONDARY_H_

//===-- quarantine.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_QUARANTINE_H_
#define SCUDO_QUARANTINE_H_

#include "bitvector.h"
#include "list.h"
#include "mutex.h"
#include "secondary.h"
#include "string_utils.h"
#include "pool.h"
#include "pthread.h"

namespace scudo {

#define MIN_HEAP_ADDR 0
#define MAX_HEAP_ADDR (((uptr)1)<<47)

struct AddrLimits {
  uptr MinAddr, MaxAddr;

  AddrLimits(): MinAddr(MAX_HEAP_ADDR), MaxAddr(MIN_HEAP_ADDR) {}
  AddrLimits(uptr MinAddr, uptr MaxAddr): MinAddr(MinAddr), MaxAddr(MaxAddr) {}
  AddrLimits(const AddrLimits& other): MinAddr(other.MinAddr), MaxAddr(other.MaxAddr) {}

  inline void combine(const AddrLimits& other) {
    if (other.MinAddr < MinAddr) MinAddr = other.MinAddr;
    if (other.MaxAddr > MaxAddr) MaxAddr = other.MaxAddr;
  }
  inline bool contains(const uptr Ptr) const { return ((Ptr >= MinAddr) && (Ptr < MaxAddr)); }
};

struct SavedSmallAlloc {
  void *Ptr;
  uptr Size;

  uptr minAddress() const { return (uptr)Ptr; }
  uptr maxAddress() const { return (uptr)Ptr+Size; }
};

template<typename T, u32 MaxCount>
class QuarantineBatch {
public:
  using ThisT = QuarantineBatch<T,MaxCount>;
  using ItemT = T;
  QuarantineBatch *Next;
  uptr Size;
  u32 Count;
  T Items[MaxCount];
  static PoolAllocator<ThisT>* Pool() {
    static PoolAllocator<ThisT> P;
    return &P;
  }

  static ThisT *getNewInstance() {
    ThisT *Instance = Pool()->alloc();
    DCHECK(Instance);
    
    Instance->init();
    return Instance;
  }

  static void deallocate(ThisT *Instance) {
    Pool()->dealloc(Instance);
  }

  void init() {
    Count = 0;
    this->Size = sizeof(QuarantineBatch<T,MaxCount>); // Account for the Batch Size.
  }

  // The total size of quarantined nodes recorded in this batch.
  uptr getQuarantinedSize() const { return Size - sizeof(QuarantineBatch); }

  void push_back(const T& Item, uptr Size) {
    DCHECK_LT(Count, MaxCount);
    Items[Count++] = Item;
    this->Size += Size;
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (uptr I = 0; I < Count; ++I)
      Result.combine(AddrLimits(Items[I].minAddress(), Items[I].maxAddress()));
    return Result;
  }

  bool canMerge(const QuarantineBatch<T,MaxCount> *const From) const {
    return Count + From->Count <= MaxCount;
  }

  void merge(QuarantineBatch<T,MaxCount> *const From) {
    DCHECK_LE(Count + From->Count, MaxCount);
    DCHECK_GE(Size, sizeof(QuarantineBatch));

    for (uptr I = 0; I < From->Count; ++I) {
      Items[Count + I] = From->Items[I];
    }
    Count += From->Count;
    Size += From->getQuarantinedSize();

    From->Count = 0;
    From->Size = sizeof(QuarantineBatch);
  }
};
  
template<typename T, u32 M>
typename QuarantineBatch<T,M>::template PoolAllocator<QuarantineBatch<T,M>> Pool;


const static u32 SmallBatchCount = 509;
const static u32 LargeBatchCount = 4064/sizeof(LargeBlock::SavedHeader); 

// Per-thread cache of memory blocks.
template<typename T, u32 MaxCount>
struct SpecificQuarantineCache {
  using BatchT = QuarantineBatch<T, MaxCount>;
  using ThisT = SpecificQuarantineCache<T, MaxCount>;

  void initLinkerInitialized() {}
  void init() {
    memset(this, 0, sizeof(*this));
    initLinkerInitialized();
  }

  // Total memory used, including internal accounting.
  uptr getSize() const { return atomic_load_relaxed(&Size); }
  // Memory used for internal accounting.
  uptr getOverheadSize() const { return List.size() * sizeof(BatchT); }

  void enqueue(const T& Item, uptr Size) {
    if (List.empty() || List.back()->Count == MaxCount) {
      BatchT *Batch = BatchT::getNewInstance();
      Batch->push_back(Item, Size);
      enqueueBatch(Batch);
    } else {
      List.back()->push_back(Item, Size);
      addToSize(Size);
    }
  }

  void transfer(SpecificQuarantineCache *From) {
    List.append_back(&From->List);
    addToSize(From->getSize());
    atomic_store_relaxed(&From->Size, 0);
  }

  void enqueueBatch(BatchT *B) {
    List.push_back(B);
    addToSize(B->Size);
  }

  BatchT *dequeueBatch() {
    if (List.empty())
      return nullptr;
    BatchT *B = List.front();
    List.pop_front();
    subFromSize(B->Size);
    return B;
  }

  void mergeBatches() {
    uptr ExtractedSize = 0;
    BatchT *Current = List.front();
    while (Current && Current->Next) {
      if (Current->canMerge(Current->Next)) {
        BatchT *Extracted = Current->Next;
        // Move all the chunks into the current batch.
        Current->merge(Extracted);
        DCHECK_EQ(Extracted->Count, 0);
        DCHECK_EQ(Extracted->Size, sizeof(BatchT));
        // Remove the next batch From the list and account for its Size.
        List.extract(Current, Extracted);
        ExtractedSize += Extracted->Size;
        // Deallocate.
        BatchT::deallocate(Extracted);
      } else {
        Current = Current->Next;
      }
    }
    subFromSize(ExtractedSize);
  }

  AddrLimits addrLimits() const {
    AddrLimits Result;
    for (const BatchT &Batch : List)
      Result.combine(Batch.addrLimits());
    return Result;
  }

  void getStats(ScopedString *Str) const {
    uptr BatchCount = 0;
    uptr TotalOverheadBytes = 0;
    uptr TotalBytes = 0;
    uptr TotalQuarantineChunks = 0;
    for (const BatchT &Batch : List) {
      BatchCount++;
      TotalBytes += Batch.Size;
      TotalOverheadBytes += Batch.Size - Batch.getQuarantinedSize();
      TotalQuarantineChunks += Batch.Count;
    }
    const uptr QuarantineChunksCapacity = BatchCount * MaxCount;
    const uptr ChunksUsagePercent =
        (QuarantineChunksCapacity == 0)
            ? 0
            : TotalQuarantineChunks * 100 / QuarantineChunksCapacity;
    const uptr TotalQuarantinedBytes = TotalBytes - TotalOverheadBytes;
    const uptr MemoryOverheadPercent =
        (TotalQuarantinedBytes == 0)
            ? 0
            : TotalOverheadBytes * 100 / TotalQuarantinedBytes;
    Str->append(
        "Stats: Quarantine: batches: %zu; bytes: %zu (user: %zu); chunks: %zu "
        "(capacity: %zu); %zu%% chunks used; %zu%% memory overhead\n",
        BatchCount, TotalBytes, TotalQuarantinedBytes, TotalQuarantineChunks,
        QuarantineChunksCapacity, ChunksUsagePercent, MemoryOverheadPercent);
  }

private:
  SinglyLinkedList<BatchT> List;
  atomic_uptr Size;

  void addToSize(uptr add) { atomic_store_relaxed(&Size, getSize() + add); }
  void subFromSize(uptr sub) { atomic_store_relaxed(&Size, getSize() - sub); }
};

struct QuarantineCache {
  using SmallCacheT = SpecificQuarantineCache<SavedSmallAlloc, SmallBatchCount>;
  using LargeCacheT = SpecificQuarantineCache<LargeBlock::SavedHeader, LargeBatchCount>;

  SmallCacheT SmallCache;
  LargeCacheT LargeCache;
  using SmallBatchT = SmallCacheT::BatchT;
  using LargeBatchT = LargeCacheT::BatchT;

  void initLinkerInitialized() {}
  void init() {
    SmallCache.init();
    LargeCache.init();
    initLinkerInitialized();
  }

  // Total memory used, including internal accounting.
  uptr getSize() const { return SmallCache.getSize() + LargeCache.getSize(); }
  // Memory used for internal accounting.
  uptr getOverheadSize() const
    { return SmallCache.getOverheadSize() + LargeCache.getOverheadSize();}

  inline void enqueueSmall(const SavedSmallAlloc& Item) {
    SmallCache.enqueue(Item, Item.Size);
  }
  inline void enqueueLarge(const LargeBlock::SavedHeader& Item) {
    LargeCache.enqueue(Item, Item.BlockEnd - Item.Block);
  }

  void transfer(QuarantineCache *From) {
    SmallCache.transfer(&From->SmallCache);
    LargeCache.transfer(&From->LargeCache);
  }

  inline void enqueueBatch(SmallBatchT *B) {
    SmallCache.enqueueBatch(B);
  }
  inline void enqueueBatch(LargeBatchT *B) {
    LargeCache.enqueueBatch(B);
  }

  inline SmallBatchT *dequeueSmallBatch() {
    return SmallCache.dequeueBatch();
  }
  inline LargeBatchT *dequeueLargeBatch() {
    return LargeCache.dequeueBatch();
  }

  void mergeBatches() {
    SmallCache.mergeBatches();
    LargeCache.mergeBatches();
  }

  AddrLimits addrLimits() const {
    AddrLimits Result = SmallCache.addrLimits();
    Result.combine(LargeCache.addrLimits());
    
    return Result;
  }

  void getStats(ScopedString *Str) const {
    SmallCache.getStats(Str);
    LargeCache.getStats(Str);
  }
};

template<typename QuarantineT>
void *sweeperThreadStart(void *QuarantinePtr) {
  QuarantineT *Quarantine = (QuarantineT*)QuarantinePtr;
  Quarantine->sweeperThreadMain();
  pthread_exit(NULL);
}

template <typename AllocatorT, typename Node> class GlobalQuarantine {
public:
  typedef QuarantineCache CacheT;
  typedef GlobalQuarantine<AllocatorT, Node> ThisT;
  typedef ShadowBitMap ShadowT;

  void initLinkerInitialized(uptr CacheSize) {
    atomic_store_relaxed(&MaxCacheSize, CacheSize);
    Cache.initLinkerInitialized();

    pthread_cond_init(&SweeperCondition, NULL);
    pthread_mutex_init(&SweeperMutex, NULL);
  }
  void init(AllocatorT *Allocator, uptr CacheSize) {
    this->Allocator = Allocator;
    CacheMutex.init();
    Cache.init();
    MaxCacheSize = {};
    initLinkerInitialized(CacheSize);
  }

  ~GlobalQuarantine() {
    killSweeperThread();
  }

  uptr getMaxSize() const {
    return SweepThreshold * Allocator->getTotalAllocatedUser() / 100;
  }
  uptr getCacheSize() const { return atomic_load_relaxed(&MaxCacheSize); }

  void put(CacheT *C, const SavedSmallAlloc& Alloc) {
    C->enqueueSmall(Alloc);
    if (C->getSize() > getCacheSize())
      drain(C);
  }
  void put(CacheT *C, const LargeBlock::SavedHeader& Header) {
    C->enqueueLarge(Header);
    if (C->getSize() > getCacheSize())
      drain(C);
  }

  void NOINLINE drain(CacheT *C) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    if (Cache.getSize() > getMaxSize())
      recycle();
  }

  void NOINLINE drainAndRecycle(CacheT *C) {
    {
      ScopedLock L(CacheMutex);
      Cache.transfer(C);
    }
    recycle();
  }

  void getStats(ScopedString *Str) const {
    // It assumes that the world is stopped, just as the allocator's printStats.
    Cache.getStats(Str);
    Str->append("Quarantine limits: global: %zuK; thread local: %zuK\n",
                getMaxSize() >> 10, getCacheSize() >> 10);
  }

  void disable() {
    CacheMutex.lock();
  }

  void enable() {
    CacheMutex.unlock();
  }

  // Point of entry for SweeperThread
  void sweeperThreadMain() {
    ShadowMap.init(MIN_HEAP_ADDR, MAX_HEAP_ADDR - MIN_HEAP_ADDR, GranuleSize);

    // Repeat until program exit
    while (true) {
      // Wait until sweep is needed or program is shutting down
      bool SweepNeeded, ShutdownNeeded;
      pthread_mutex_lock(&SweeperMutex);
      while (true) {
        ShutdownNeeded = ShutdownSignal;
        SweepNeeded = Cache.getSize() > getMaxSize();
        if (ShutdownNeeded | SweepNeeded)
          break;
        pthread_cond_wait(&SweeperCondition, &SweeperMutex);
      }
      pthread_mutex_unlock(&SweeperMutex);

      if (ShutdownNeeded)
        return; // Kill thread

      DCHECK(SweepNeeded);
      doSweepAndRecycle();
    }
  }

private:
  // Read-only data.
  alignas(SCUDO_CACHE_LINE_SIZE) HybridMutex CacheMutex;
  CacheT Cache;
  AllocatorT *Allocator;
  atomic_uptr MaxSize;
  alignas(SCUDO_CACHE_LINE_SIZE) atomic_uptr MaxCacheSize;
  const uptr SweepThreshold = /* Sweep when */25/* % of all allocated memory is quarantined. */;
  const uptr GranuleSize = 32/* Bytes */; 
  // Sweeper thread
  pthread_t SweeperThread;
  volatile bool SweeperThreadLaunched;
  pthread_mutex_t SweeperMutex;
  pthread_cond_t SweeperCondition;
  volatile bool ShutdownSignal;
  ShadowT ShadowMap;

  void killSweeperThread() {
    pthread_mutex_lock(&SweeperMutex);
      ShutdownSignal = true;
    pthread_mutex_unlock(&SweeperMutex);
  }

  void recycle() {
    if (!pthread_mutex_trylock(&SweeperMutex)) {
      // Launch thread here. We use late initialisation for this to avoid deadlock in init()
      if (UNLIKELY(!SweeperThreadLaunched)) {
        SweeperThreadLaunched = true;
        pthread_create(&SweeperThread, nullptr, &sweeperThreadStart<ThisT>, this);
      }

      pthread_cond_signal(&SweeperCondition);
      pthread_mutex_unlock(&SweeperMutex);
    }
  }

  void NOINLINE doSweepAndRecycle() {
    CacheT ToCheck = gatherContents();
    
    AddrLimits PointerLimits = ToCheck.addrLimits();
    doSweepAndMark(PointerLimits);

    CacheT FailedFrees;
    FailedFrees.init();
    
    recycleUnmarked(FailedFrees, ToCheck);
    CacheT NewlyQuarantined = gatherContents();
    recycleUnmarked(FailedFrees, NewlyQuarantined, /*CheckRange=*/true, PointerLimits);

    Cache.transfer(&FailedFrees); // Reinsert failed frees
    ShadowMap.clear();
  }

  inline void doSweepAndMark(const AddrLimits& Limits) {
    auto MarkingLambda = [&](uptr Ptr, uptr Size) -> void {
      uptr* const Begin = (uptr*) Ptr;
      uptr* const End = (uptr*) (Ptr + Size);
      for (uptr *Word = Begin; Word < End; Word++) {
        uptr Target = *Word;
        // Only mark if Target is in the plausible range
        if (!Limits.contains(Target))
            continue;
        ShadowMap.set(Target);
      }
    };

    Allocator->iterateOverActiveMemory(MarkingLambda);
  }

  inline CacheT gatherContents() {
    CacheT Result;
    Result.init();
    {
      ScopedLock L(CacheMutex);
      // Go over the batches and merge partially filled ones to
      // save some memory.
      const uptr CacheSize = Cache.getSize();
      const uptr OverheadSize = Cache.getOverheadSize();
      DCHECK_GE(CacheSize, OverheadSize);
      // Do the merge only when overhead exceeds this predefined limit (might
      // require some tuning). It saves us merge attempt when the batch list
      // quarantine is unlikely to contain batches suitable for merge.
      constexpr uptr OverheadThresholdPercents = 100;
      if (OverheadSize * (100 + OverheadThresholdPercents) >
              CacheSize * OverheadThresholdPercents) {
        Cache.mergeBatches();
      }

      // Remove batches from cache
      Result.transfer(&Cache);
    }

    return Result;
  }

  template<class SpecificCacheT, typename SF, typename FF>
  void checkUnmarked(SpecificCacheT &ToCheck, SF SuccessCb, FF FailureCb,
                     bool CheckRange, AddrLimits Limits) {
    while (typename SpecificCacheT::BatchT *B = ToCheck.dequeueBatch()) {
      constexpr uptr NumberOfPrefetch = 8UL;
      CHECK(NumberOfPrefetch <= ARRAY_SIZE(B->Items));
      for (uptr I = 0; I < NumberOfPrefetch; I++) PREFETCH(&B->Items[I]);
      for (uptr I = 0, Count = B->Count; I < Count; I++) {
        if (I + NumberOfPrefetch < Count) PREFETCH(&B->Items[I + NumberOfPrefetch]);

        auto& Item = B->Items[I];
        uptr Start = Item.minAddress();
        uptr End = Item.maxAddress();
        uptr Size = End - Start;
        CHECK(Start);

        // If something was inserted during the sweep, and it lies outside of the range
        // of pointers we considered, then fail to free it, and re-check on the next sweep
        bool OutOfRange = CheckRange
                              && ((Item.minAddress() < Limits.MinAddr)
                                    || (Item.maxAddress() > Limits.MaxAddr));

        if (OutOfRange || marked(Start, Size)) {
          FailureCb(Item);
        } else {
          SuccessCb(Item);
        }
      }
      SpecificCacheT::BatchT::deallocate(B);
    }
  }

  inline void recycleUnmarked(CacheT &FailedFrees, CacheT &ToCheck, bool CheckRange = false,
                                AddrLimits Limits = AddrLimits()) {
    auto SmallFailureCb =
      [&FailedFrees](const SavedSmallAlloc& Item) -> void { FailedFrees.enqueueSmall(Item); };
    auto SmallSuccessCb = [this](const SavedSmallAlloc& Item) -> void {
      Allocator->recycleChunk(reinterpret_cast<Node*>(Item.Ptr));
    };
    auto LargeFailureCb = [&FailedFrees](const LargeBlock::SavedHeader& Item) -> void {
      FailedFrees.enqueueLarge(Item);
    };
    auto LargeSuccessCb = [this](const LargeBlock::SavedHeader& Item) -> void {
      Allocator->recycleChunk(Item);
    };
    
    checkUnmarked(ToCheck.SmallCache, SmallSuccessCb, SmallFailureCb, CheckRange, Limits);
    checkUnmarked(ToCheck.LargeCache, LargeSuccessCb, LargeFailureCb, CheckRange, Limits);
  }

  inline bool marked(uptr Ptr, uptr Size) {
    return !ShadowMap.allZero(Ptr, Ptr+Size);
  }
};

} // namespace scudo

#endif // SCUDO_QUARANTINE_H_

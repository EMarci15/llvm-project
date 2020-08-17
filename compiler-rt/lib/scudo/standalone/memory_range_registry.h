//===-- memory_range_registry.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_MEMORY_RANGE_REGISTRY_H_
#define SCUDO_MEMORY_RANGE_REGISTRY_H_

#include "list.h"
#include "mutex.h"
#include "proc_maps_parse.h"
#include <fcntl.h>
#include <unistd.h>

namespace scudo {

// Code registering and storing memory ranges mapped outside of the allocator
// that could contain heap pointers.
// Mutable sections of .data and .bss are obtained by parsing /proc/self/maps at startup,
// the stack of each thread is estimated periodically.

#define MEMRANGE_SIZE 512

struct MemRange {
  uptr Begin, End;
  MemRange(): Begin(0), End(0) {}
  MemRange(MemRange& Other): Begin(Other.Begin), End(Other.End) {}
  MemRange(uptr Begin, uptr End): Begin(Begin), End(End) {}
};

class MemoryRangeRegistry {
public:
  void init() {
    RangeCount = 0;
  }

  template<typename F>
  void iterateRanges(F Callback) {
    initMaybe();

    for (uptr i=0; i<RangeCount; i++)
      Callback(Ranges[i].Begin, Ranges[i].End-Ranges[i].Begin);
  }

  void registerStack(uptr& RangeIndex) {
    uptr PageSize = getPageSizeCached();
    volatile uptr localVar = 1;

    if (UNLIKELY(!RangeIndex)) {
      initMaybe();
      ScopedLock L(Mutex);
      RangeIndex = RangeCount++;
      Ranges[RangeIndex].Begin = roundDownTo((uptr)&localVar, PageSize);
      Ranges[RangeIndex].End = Ranges[RangeIndex].Begin+PageSize;
      return;
    }

    Ranges[RangeIndex].Begin = roundDownTo((uptr)&localVar, PageSize);
  }
private:
  HybridMutex Mutex;
  uptr RangeCount;
  MemRange Ranges[MEMRANGE_SIZE];

  // Use late init, as this will call malloc() internally to perform I/O.
  void initMaybe() {
    if (LIKELY(RangeCount))
      return;
    memset(this, 0, sizeof(this));

    registerMapRanges();
    DCHECK_GT(RangeCount, 0);
  }

  void registerMapRanges();
};

void MemoryRangeRegistry::registerMapRanges() {
    ScopedLock L(Mutex);
    int fmaps = open("/proc/self/maps", O_RDONLY);
    CHECK(fmaps >= 0);
    char buff[255];
    char *e = buff;
    char *b = buff;

    while (true) {
      char *le = b;
      while ((le < e) && (*le != '\n')) le++;

      if (le == e) {
        char *x = buff;
        for (char *y = b; y < e; x++, y++)
          *x = *y;
        le = e = x;
        b = buff;

        e += read(fmaps, le, (&buff[255]-le));
        while ((le < e) && (*le != '\n')) le++;
        if (le == e) break;
      }

      ProcRegion pr(b);
      b = le+1;

      // Do not scan regions with special protection (unused or not containing heap ptrs)
      if (!pr.readable || !pr.writable || pr.executable) continue;

      // Heap regions should be handled via extent registration (managed) to avoid
      //     scanning jemalloc metadata, ngc regions and double-scanning
      if ((!pr.has_path) || (pr.heap) || (pr.stack)) continue;

      // Do not scan shared mappings to mmap'd files (should not contain pointers)
      // Note: .text .bss and .data are mapped as private file mappings
      if (!pr.CoW) continue;

      // Ignore large mappings (these likely just take advantage of overcommitting)
      if ((pr.end_ptr - pr.start_ptr) > (1ll << 30)/*1GB*/) continue;

      // Ignore regions mapped from jemalloc. These may be for metadata that should be
      // conceptually non-garbage-collected.
      if (pr.from_jemalloc) continue;

      Ranges[RangeCount++] = MemRange(pr.start_ptr, pr.end_ptr);
    }

    close(fmaps);
}

} // namespace scudo

#endif // SCUDO_MEMORY_RANGE_REGISTRY_H_

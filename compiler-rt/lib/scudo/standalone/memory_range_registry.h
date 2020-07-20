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
#include "proc_maps_parse.h"
#include "stdio.h"

namespace scudo {

// Code registering and storing memory ranges mapped outside of the allocator
// that could contain heap pointers.
// Mutable sections of .data and .bss are obtained by parsing /proc/self/maps at startup,
// the stack of each thread is estimated periodically.

#define MEMRANGE_SIZE 256

struct MemRange {
  uptr Begin, Size;
  MemRange(): Begin(0), Size(0) {}
  MemRange(MemRange& Other): Begin(Other.Begin), Size(Other.Size) {}
  MemRange(uptr Begin, uptr Size): Begin(Begin), Size(Size) {}
};

static void registerMapRanges(MemRange *Ranges, uptr& Size);

class MemoryRangeRegistry {
public:
  void init() {
    RangeCount = 0;
  }

  template<typename F>
  void iterateRanges(F Callback) {
    initMaybe();

    for (uptr i=0; i<RangeCount; i++)
      Callback(Ranges[i].Begin, Ranges[i].Size);

    // TODO(marton) iterate stacks
  }
private:
  uptr RangeCount;
  MemRange Ranges[MEMRANGE_SIZE];

  // Use late init, as this will call malloc() internally to perform I/O.
  void initMaybe() {
    if (LIKELY(RangeCount))
      return;

    registerMapRanges(Ranges, RangeCount);
    DCHECK_GT(RangeCount, 0);
  }
};

void registerMapRanges(MemRange *Ranges, uptr& Size) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char buff[255];
    while (fgets(buff, sizeof(buff), maps) != NULL) {
        ProcRegion pr(buff);

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

        Ranges[Size++] = MemRange(pr.start_ptr, pr.end_ptr-pr.start_ptr);
    }

    fclose(maps);
}

} // namespace scudo

#endif // SCUDO_MEMORY_RANGE_REGISTRY_H_

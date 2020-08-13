//===-- pool.h --------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_POOL_H_
#define SCUDO_POOL_H_

#include "internal_defs.h"
#include "list.h"

namespace scudo {

// A very simple, low-performance pool allocator for internal structures.
// This is designed for structures spanning a small number of pages. Using this
// reduces the number of mmap mappings.

template<typename T>
class PoolAllocator {
  struct FreeItem { FreeItem* Next; };

  uptr ElementAllocSize, AllocCount;

  void expand() {
    uptr Addr = (uptr)map(NULL, AllocCount*ElementAllocSize, "scudo:pool");
    for (uptr i = 0; i < AllocCount; i++) {
      uptr Ptr = Addr + i*ElementAllocSize;
      FreeList.push_front((FreeItem*)Ptr);
    }
    
    // Increase adaptively
    AllocCount *= 2;
  }

  public:
  void initLinkerInitialized() {
    const uptr PageSize = getPageSizeCached();
    AllocCount = 8;
    ElementAllocSize = roundUpTo(sizeof(T), PageSize);
  }

  void init() { initLinkerInitialized(); }

  PoolAllocator() { init(); }

  T *alloc() {
    ScopedLock L(Mutex);
    if (FreeList.empty())
      expand();
    FreeItem *I = FreeList.front();
    FreeList.pop_front();

    return (T*)I;
  }

  void dealloc(T* Item) {
    ScopedLock L(Mutex);
    FreeList.push_front((FreeItem*)Item);
  }

  private:
  HybridMutex Mutex;
  SinglyLinkedList<FreeItem> FreeList;
  bool Initialized = false;
};

} // namespace scudo

#endif // SCUDO_POOL_H_

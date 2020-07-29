//===-- proc_maps_parse.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_PROC_MAPS_PARSE_H_
#define SCUDO_PROC_MAPS_PARSE_H_

#include "internal_defs.h"

namespace scudo {

// Code to parse lines from /proc/self/maps to find statically mapped regions.

struct ProcRegion {
    uptr start_ptr;
    uptr end_ptr;
    bool readable, writable, executable, CoW;
    uptr offset;
    // const char *dev;
    uptr inode;
    // const char *pathname;
    bool has_path;
    bool from_jemalloc;
    bool stack;
    bool heap;

    // Construct object by parsing a line of /proc/self/maps passed as a char buffer
    ProcRegion(char *line);
};

} // namespace scudo

#endif // SCUDO_PROC_MAPS_PARSE_H_

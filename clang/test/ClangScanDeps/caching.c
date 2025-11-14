// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: mkdir %t/cwd && cd %t/cwd

//--- include/header.h
//--- tu.c
#include "header.h"

// RUN: clang-scan-deps -format experimental-include-tree-full -cas-path %t/cas -- \
// RUN:   %clang -c %t/tu.c -o %t/tu.o -I %t/include 2> %t/first
// RUN: FileCheck --input-file=%t/first %s --check-prefix=CHECK_MISS
// CHECK_MISS: action cache has no entries
//
// RUN: clang-scan-deps -format experimental-include-tree-full -cas-path %t/cas -- \
// RUN:   %clang -c %t/tu.c -o %t/tu.o -I %t/include 2> %t/second
// RUN: FileCheck --input-file=%t/second %s --check-prefix=CHECK_HIT
// CHECK_HIT: Scanning cache hit
//
// RUN: touch %t/include/header2.h
//
// RUN: clang-scan-deps -format experimental-include-tree-full -cas-path %t/cas -- \
// RUN:   %clang -c %t/tu.c -o %t/tu.o -I %t/include 2> %t/first
// RUN: FileCheck --input-file=%t/first %s --check-prefix=CHECK_MISS_INCREMENTAL
// CHECK_MISS_INCREMENTAL: action cache has no entries

// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: mkdir %t/cwd && cd %t/cwd

//--- sysroot/.keep
//--- include/header.h
//--- src/tu.c
#include "header.h"

// RUN: clang-scan-deps -format experimental-include-tree-full -o %t/deps1.json -cas-path %t/cas -- \
// RUN:   %clang -c %t/src/tu.c -o %t/tu.o -I %t/include -isysroot %t/sysroot 2> %t/first
// RUN: FileCheck --input-file=%t/first %s --check-prefix=CHECK_MISS
// CHECK_MISS: remark: scan job cache miss for 'llvmcas:{{.*}}'
//
// RUN: clang-scan-deps -format experimental-include-tree-full -o %t/deps2.json -cas-path %t/cas -- \
// RUN:   %clang -c %t/src/tu.c -o %t/tu.o -I %t/include -isysroot %t/sysroot 2> %t/second
// RUN: FileCheck --input-file=%t/second %s --check-prefix=CHECK_HIT
// CHECK_HIT: remark: scan job cache hit for 'llvmcas://{{.*}}' => 'llvmcas://{{.*}}'
// RUN: diff %t/deps1.json %t/deps2.json
//
// RUN: touch %t/include/header2.h
//
// RUN: clang-scan-deps -format experimental-include-tree-full -o %t/deps3.json -cas-path %t/cas -- \
// RUN:   %clang -c %t/src/tu.c -o %t/tu.o -I %t/include -isysroot %t/sysroot 2> %t/third
// RUN: FileCheck --input-file=%t/third %s --check-prefix=CHECK_MISS_INCREMENTAL
// CHECK_MISS_INCREMENTAL: remark: scan job cache miss for 'llvmcas:{{.*}}'

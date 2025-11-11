// RUN: rm -rf %t
// RUN: split-file %s %t

//--- include/unused.h
//--- include/used.h
//--- tu.c
#include "used.h"

// RUN: clang-scan-deps -format experimental-include-tree-full -- %clang -c %t/tu.c -I %t/include

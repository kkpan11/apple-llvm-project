// RUN: rm -rf %t
// RUN: split-file %s %t

// Make sure that the working directory is picked up when caching.

// RUN: cd %t && %clang -cc1depscan -fdepscan=inline -o %t/t.rsp -cc1-args \
// RUN:   -cc1 -triple x86_64-apple-macos12 -fcas-path %t/cas -emit-obj %t/main.c -o %t/t.o -Iinclude

// RUN: cd %t && %clang @%t/t.rsp -index-store-path %t/idx
// RUN: c-index-test core -print-unit %t/idx | FileCheck %s -DDIR=%t

// CHECK: main-path: [[DIR]]{{/|\\}}main.c
// CHECK: work-dir: [[DIR]]
// CHECK: Record | user | {{.*}}include{{/|\\}}foo.h

//--- main.c
#include "foo.h"
int main(void) { return foo(); }

//--- include/foo.h
int foo(void);

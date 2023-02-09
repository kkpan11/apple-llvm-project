// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s
// This comment block simulate a file header comment.
// CHECK: fix-it:"{{.*}}warn-unsafe-buffer-usage-fixits-include-header.cpp":{5:1-5:1}:"#include <span>\n"

void foo() {
  /* just some other code */
}

void local_array_subscript_simple() {
  int tmp;
  int *p = new int[10];
  tmp = p[5];
}

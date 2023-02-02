// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s

// CHECK-NOT: fix-it:

// There already is an overload.
void local_array_subscript_simple();

void local_array_subscript_simple(int *p) {
  int tmp;
  tmp = p[5];
}

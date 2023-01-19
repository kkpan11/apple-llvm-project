// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage %s

void foo() {
  int* ptr;
  *(ptr + 5);
}
// RUN: cp %s %t.cpp
// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fixit %t.cpp
// RUN: grep -v CHECK %t.cpp | FileCheck %s

// TODO cases where we don't want fixits

// The Fix-It for unsafe operation is trivially empty.
// In order to test that our machinery recognizes that we can test if the variable declaration gets a Fix-It.
// If the operation wasn't handled propertly the declaration won't get Fix-It.
// By testing presence of the declaration Fix-It we indirectly test presence of the trivial Fix-It for its operations.
void test() {
  int *p = new int[10];
  // CHECK: std::span<int> p {new int[10], 10};
  p[5] = 1;
  // CHECK: p[5] = 1;
}

// RUN: cp %s %t.cpp
// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fixit %t.cpp
// RUN: grep -v CHECK %t.cpp | FileCheck %s

void foo(...);
typedef int * Int_ptr_t;
typedef int Int_t;

void local_array_subscript_simple() {
// CHECK: std::span<int> p{new int [10], 10};
// CHECK: p[5] = 5;
  int *p = new int[10];
  p[5] = 5;

// CHECK: std::span<const int> q{new int [10], 10};
// CHECK: std::span<int> x{new int [10], 10};
// CHECK: std::span<int> y{new int, 1};
// CHECK: std::span<Int_t> z{new int [10], 10};
// CHECK: std::span<Int_t> w{new Int_t [10], 10};
// CHECK: foo(q[5], x[5], y[5], z[5], w[5]);
  const int *q = new int[10];
  Int_ptr_t x = new int[10];
  Int_ptr_t y = new int;
  Int_t * z = new int[10];
  Int_t * w = new Int_t[10];
  foo(q[5], x[5], y[5], z[5], w[5]);
  // y[5] will crash after being span
}

void local_array_subscript_auto() {
// CHECK: std::span<int> p{new int [10], 10};
// CHECK: p[5] = 5;
  auto p = new int[10];
  p[5] = 5;
}

void local_array_subscript_variable_extent() {
  int n = 10;
// CHECK: std::span<int> p{new int [n], n};
// CHECK: std::span<int> q{new int [n++], ...};
// CHECK: foo(p[5], q[5]);
  int *p = new int[n];
  // If the extent expression does not have a constant value, we cannot fill the extent for users...
  int *q = new int[n++];
  foo(p[5], q[5]);
}


void local_ptr_to_array() {
  int n = 10;
  int a[10];
  int b[n];  // If the extent expression does not have a constant value, we cannot fill the extent for users...
// CHECK: std::span<int> p{a, 10};
// CHECK: std::span<int> q{b, ...};
// CHECK: foo(p[5], q[5]);
  int *p = a;
  int *q = b;
  foo(p[5], q[5]);
}

void local_ptr_addrof_init() {
  int a[10];
  int var;
// CHECK: std::span<int[10]> p{&a, 1};
// CHECK: std::span<int> q{&var, 1};
// CHECK: foo(p[5], q[5]);
  int (*p)[10] = &a;
  int * q = &var;
  // These two expressions involve unsafe buffer accesses, which will
  // crash at runtime after applying the fix-it,
  foo(p[5], q[5]);
}

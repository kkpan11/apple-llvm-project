// RUN: cp %s %t.cpp
// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fixit %t.cpp
// RUN: grep -v CHECK %t.cpp | FileCheck %s
typedef int * Int_ptr_t;
typedef int Int_t;

void local_array_subscript_simple() {
  int tmp;
// CHECK: std::span<int> p {new int[10], 10};
// CHECK: std::span<const int> q {new int[10], 10};
// CHECK: tmp = p[5];
// CHECK: tmp = q[5];
  int *p = new int[10];
  const int *q = new int[10];
  tmp = p[5];
  tmp = q[5];

// CHECK: std::span<int> x {new int[10], 10};
// CHECK: std::span<int> y {new int, 1};
// CHECK: std::span<Int_t> z {new int[10], 10};
// CHECK: std::span<Int_t> w {new Int_t[10], 10};

  Int_ptr_t x = new int[10];
  Int_ptr_t y = new int;
  Int_t * z = new int[10];
  Int_t * w = new Int_t[10];

  // CHECK: tmp = x[5];
  tmp = x[5];
  // CHECK: tmp = y[5];
  tmp = y[5]; // y[5] will crash after being span
  // CHECK: tmp = z[5];
  tmp = z[5];
  // CHECK: tmp = w[5];
  tmp = w[5];
}

void local_array_subscript_auto() {
  int tmp;
// CHECK: std::span<int> p {new int[10], 10};
// CHECK: tmp = p[5];
  auto p = new int[10];
  tmp = p[5];
}

void local_array_subscript_variable_extent() {
  int n = 10;
  int tmp;

  // CHECK: std::span<int> p {new int[n], n};
  // CHECK: std::span<int> q {new int[n++], <# placeholder #>};
  // CHECK: tmp = p[5];
  // CHECK: tmp = q[5];
  int *p = new int[n];
  // If the extent expression does not have a constant value, we cannot fill the extent for users...
  int *q = new int[n++];
  tmp = p[5];
  tmp = q[5];
}


void local_ptr_to_array() {
  int tmp;
  int n = 10;
  int a[10];
  int b[n];  // If the extent expression does not have a constant value, we cannot fill the extent for users...
  // CHECK: std::span<int> p {a, 10};
  // CHECK: std::span<int> q {b, <# placeholder #>};
  // CHECK: tmp = p[5];
  // CHECK: tmp = q[5];
  int *p = a;
  int *q = b;
  tmp = p[5];
  tmp = q[5];
}

void local_ptr_addrof_init() {
  int var;
// CHECK: std::span<int> q {&var, 1};
// CHECK: var = q[5];
  int * q = &var;
  // This expression involves unsafe buffer accesses, which will crash
  // at runtime after applying the fix-it,
  var = q[5];
}

void decl_without_init() {
  int tmp;
  // CHECK: std::span<int> p;
  int * p;
  // CHECK: std::span<int> q;
  Int_ptr_t q;

  tmp = p[5];
  tmp = q[5];
}

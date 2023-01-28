// RUN: cp %s %t.cpp
// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fsyntax-only -fixit %t.cpp
// RUN: grep -v CHECK %t.cpp | FileCheck %s

// TODO test we don't mess up vertical whitespace
// TODO test different whitespaces
// TODO test different contexts
  // when it's on the right side

void basic() {
  int *ptr;
  *(ptr+5)=1;
  // CHECK: ptr[5]=1;
}

// The weird preceding semicolon ensures that we preserve that range intact.
void char_ranges() {
  int *p;
  ;* ( p + 5 ) = 1;
  // CHECK: ;p[5] = 1;
  ;*   (p+5)= 1;
  // CHECK: ;p[5]= 1;
  ;*(   p+5)= 1;
  // CHECK: ;p[5]= 1;
  ;*(   p+5)= 1;
  // CHECK: ;p[5]= 1;
  ;*( p   +5)= 1;
  // CHECK: ;p[5]= 1;
  ;*(p+   5)= 1;
  // CHECK: ;p[5]= 1;
  ;*(p+ 5   )= 1;
  //CHECK: ;p[5]= 1;
  ;*(p+ 5)   = 1;
  // CHECK: ;p[5]   = 1;
  ;   *(p+5)= 1;
  // CHECK: ;   p[5]= 1;

  ;*(p+123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*   (p+123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*(   p+123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*(   p+123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*(p   +123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*(p+   123456)= 1;
  // CHECK: ;p[123456]= 1;
  ;*(p+123456   )= 1;
  // CHECK: ;p[123456]= 1;
  ;*(p+123456)   = 1;
  // CHECK: ;p[123456] =   1;

  int *ptrrrrrr;
  ;* ( ptrrrrrr + 123456 )= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*   (ptrrrrrr+123456)= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(   ptrrrrrr+123456)= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(   ptrrrrrr+123456)= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(ptrrrrrr   +123456)= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(ptrrrrrr+   123456)= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(ptrrrrrr+123456   )= 1;
  // CHECK: ;ptrrrrrr[123456]= 1;
  ;*(ptrrrrrr+123456)   = 1;
  // CHECK: ;ptrrrrrr[123456]   = 1;
}

// Fixits emitted for the cases below would be incorrect.
// CHECK-NOT: [
// Array subsctipt opertor of std::span accepts unsigned integer.
void negative() {
  int* ptr;
  *(ptr + -5) = 1; // skip
}

void subtraction() {
  int* ptr;
  *(ptr - 5) = 1; // skip
}

void subtraction_of_negative() {
  int* ptr;
  *(ptr - -5) = 1; // FIXME: implement fixit (uncommon case - low priority)
}

void base_on_rhs() {
  int* ptr;
  *(10 + ptr) = 1; // FIXME: not implemented yet
}

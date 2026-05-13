// Test this without pch.
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -std=c++26 -include %S/Inputs/tmo_allocation_cxx.h -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -Rtmo-remarks -verify -fsyntax-only \
// RUN:                                             -std=c++26 -include %S/Inputs/tmo_allocation_cxx.h -o - %s

// Test with pch.
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -std=c++26 -emit-pch -o %t %S/Inputs/tmo_allocation_cxx.h
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -std=c++26 -include-pch %t -emit-llvm -o - %s | FileCheck %s

// Test with pch and remarks.
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -Rtmo-remarks -std=c++26 -emit-pch -o %t %S/Inputs/tmo_allocation_cxx.h
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -Rtmo-remarks -std=c++26 -include-pch %t -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -x c++ -ftyped-memory-operations -Wtyped-memory-inference-failure -Rtmo-remarks -verify -fsyntax-only \
// RUN:                                                         -std=c++26 -include-pch %t %s


// RUN: %clang_cc1 -x c++ -std=c++23 -ftyped-memory-operations -Wtyped-memory-inference-failure -emit-pch -fpch-instantiate-templates -o %t %S/Inputs/tmo_allocation_cxx.h
// RUN: %clang_cc1 -x c++ -std=c++23 -ftyped-memory-operations -Wtyped-memory-inference-failure -include-pch %t -emit-llvm -o - %s  | FileCheck %s


static void call_in_pch_function(void) {
  in_pch_function();
}
// CHECK-LABEL: in_pch_function
// CHECK: call ptr @typed_malloc(i64 noundef 1000, i64 noundef [[LOC1_DESC:[0-9]+]])
// CHECK: call ptr @typed_malloc(i64 noundef 4, i64 noundef [[GENERICDATA32_DESC:72057870300512784]])
// CHECK: call ptr @typed_malloc(i64 noundef 4, i64 noundef [[GENERICDATA32_DESC]])
// CHECK: call ptr @typed_malloc(i64 noundef 100, i64 noundef [[GENERICDATA32_DESC]])
// CHECK: call ptr @typed_malloc(i64 noundef 24, i64 noundef [[S1_DESC:74309672738655766]])
// CHECK: call ptr @typed_malloc(i64 noundef 24, i64 noundef [[S1_DESC]])
// CHECK: call ptr @typed_malloc(i64 noundef 100, i64 noundef [[S1_DESC]])

// CHECK-LABEL: in_pch_template_function
// CHECK: call ptr @typed_malloc(i64 noundef 1000, i64 noundef [[LOC2_DESC:[0-9]+]])

// CHECK-LABEL: out_of_pch_function
void out_of_pch_function() {
  void *iptr1 = malloc(sizeof(int)); // #iptr1
  // CHECK: call ptr @typed_malloc(i64 noundef 4, i64 noundef [[GENERICDATA32_DESC:72057870300512784]])
  // expected-remark@#iptr1 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#iptr1 {{inferred 'int' from expression 'sizeof(int)'}}
  // expected-note@#iptr1 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  int *iptr2 = (int *)malloc(sizeof(int)); // #iptr2
  // CHECK: call ptr @typed_malloc(i64 noundef 4, i64 noundef [[GENERICDATA32_DESC]])
  // expected-remark@#iptr2 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#iptr2 {{inferred 'int' from expression 'sizeof(int)'}}
  // expected-note@#iptr2 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  int *iptr3 = (int *)malloc(10); // #iptr3
  // CHECK: call ptr @typed_malloc(i64 noundef 10, i64 noundef [[GENERICDATA32_DESC]])
  // expected-remark@#iptr3 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#iptr3 {{inferred 'int' from cast of result from call to '(int *)malloc(10)'}}
  // expected-note@#iptr3 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  void *s1ptr1 = malloc(sizeof(struct S1)); // #s1ptr1
  // CHECK: call ptr @typed_malloc(i64 noundef 24, i64 noundef [[S1_DESC]])
  // expected-remark@#s1ptr1 {{passing TMO information for type 'struct S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#s1ptr1 {{inferred 'struct S1' from expression 'sizeof(struct S1)'}}
  // expected-note@#s1ptr1 {{encoding 'struct S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
  struct S1 *s1ptr2 = (struct S1 *)malloc(sizeof(struct S1)); // #s1ptr2
  // CHECK: call ptr @typed_malloc(i64 noundef 24, i64 noundef [[S1_DESC]])
  // expected-remark@#s1ptr2 {{passing TMO information for type 'struct S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#s1ptr2 {{inferred 'struct S1' from expression 'sizeof(struct S1)'}}
  // expected-note@#s1ptr2 {{encoding 'struct S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
  struct S1 *s1ptr3 = (struct S1 *)malloc(100); // #s1ptr3
  // CHECK: call ptr @typed_malloc(i64 noundef 100, i64 noundef [[S1_DESC]])
  // expected-remark@#s1ptr3 {{passing TMO information for type 'struct S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#s1ptr3 {{inferred 'struct S1' from cast of result from call to '(struct S1 *)malloc(100)'}}
  // expected-note@#s1ptr3 {{encoding 'struct S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
}

// CHECK: !{!"type-descriptor", !"[[LOC1_DESC]]", !"[[LOC1_DESC]]", !"\22LayoutSemantics\22: [ ], \22TypeFlags\22: [ ], \22TypeKind\22: \22KindC\22, \22CallsiteFlags\22: [ ]"}
// CHECK: !{!"type-descriptor", !"[[GENERICDATA32_DESC]]", !"1384677904", !"\22LayoutSemantics\22: [ \22GenericData\22 ], \22TypeFlags\22: [ ], \22TypeKind\22: \22KindC\22, \22CallsiteFlags\22: [ \22FixedSize\22 ]"}
// CHECK: !{!"type-descriptor", !"[[S1_DESC]]", !"4009135638", !"\22LayoutSemantics\22: [ \22AnonymousPointer\22, \22GenericData\22 ], \22TypeFlags\22: [ ], \22TypeKind\22: \22KindC\22, \22CallsiteFlags\22: [ \22FixedSize\22 ]"}
// CHECK: !{!"type-descriptor", !"[[LOC2_DESC]]", !"[[LOC2_DESC]]", !"\22LayoutSemantics\22: [ ], \22TypeFlags\22: [ ], \22TypeKind\22: \22KindC\22, \22CallsiteFlags\22: [ ]"}

// non-tmo-tests

void call_non_tmo_functions() {
  void *r1 = in_pch_non_tmo_call();
  void *r2 = in_pch_non_tmo_call_inline();
}

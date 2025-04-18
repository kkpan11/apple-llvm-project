// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -verify -fptrauth-intrinsics -std=c++11 %s
// RUN: %clang_cc1 -triple aarch64-linux-gnu -fsyntax-only -verify -fptrauth-intrinsics -std=c++11 %s

#if __has_extension(ptrauth_restricted_intptr_qualifier)

template <typename T> struct S {
  T __ptrauth_restricted_intptr(0,0,1234) test;
  // expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'void *' is invalid}}
  // expected-error@-2{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'int' is invalid}}
  // expected-error@-3 3 {{type '__ptrauth_restricted_intptr(0,0,1234) T' is already __ptrauth_restricted_intptr-qualified}}
};

void f1() {
  S<__INTPTR_TYPE__> basic;
  S<int> invalid_type;
  // expected-note@-1{{in instantiation of template class 'S<int>' requested here}}
  S<void *> mismatched_pointer_type;
  // expected-note@-1{{in instantiation of template class 'S<void *>' requested here}}
  S<void *__ptrauth_restricted_intptr(0,0,1234)> mismatched_pointer_type_incorrect_ptrauth1;
  // expected-error@-1 {{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'void *' is invalid}}
  S<void *__ptrauth(0,0,1234)> mismatched_pointer_type_correct_ptrauth;
  // expected-note@-1{{in instantiation of template class 'S<void *__ptrauth(0,0,1234)>' requested here}}
  S<__INTPTR_TYPE__ __ptrauth_restricted_intptr(0,0,1234)> matched;
  // expected-note@-1{{in instantiation of template class 'S<__ptrauth_restricted_intptr(0,0,1234) long>'}}
  S<__INTPTR_TYPE__ __ptrauth_restricted_intptr(0,0,1235)> mismatching_qualifier1;
  // expected-note@-1{{in instantiation of template class 'S<__ptrauth_restricted_intptr(0,0,1235) long>' requested here}}
  S<__INTPTR_TYPE__ __ptrauth(0,0,1234)> mismatching_qualifier2;
  // expected-error@-1{{'__ptrauth' qualifier only applies to pointer types; 'long' is invalid}}
  S<__INTPTR_TYPE__ __ptrauth(0,0,1235)> mismatching_qualifier3;
  // expected-error@-1{{'__ptrauth' qualifier only applies to pointer types; 'long' is invalid}}
};

void f2() {
  S<__INTPTR_TYPE__> unqualified;
  S<__INTPTR_TYPE__ __ptrauth_restricted_intptr(0,0,1234)> qualified;
  __INTPTR_TYPE__ __ptrauth_restricted_intptr(0,0,1234)* p;
  p = &unqualified.test;
  p = &qualified.test;
  __INTPTR_TYPE__ *mismatch;
  mismatch = &unqualified.test;
  // expected-error@-1{{assigning '__ptrauth_restricted_intptr(0,0,1234) long *' to 'long *' changes pointer authentication of pointee type}}
  mismatch = &qualified.test;
}

template <typename T> struct G {
  T __ptrauth(0,0,1234) test;
  // expected-error@-1 3 {{type '__ptrauth(0,0,1234) T' is already __ptrauth-qualified}}
};

template <typename T> struct Indirect {
  G<T> layers;
  // expected-note@-1{{in instantiation of template class 'G<void *__ptrauth(0,0,1235)>' requested here}}
  // expected-note@-2{{in instantiation of template class 'G<void *__ptrauth(ptrauth_key_none)>' requested here}}
  // expected-note@-3{{in instantiation of template class 'G<void *__ptrauth(0,0,1234)>' requested here}}
};

template <int K, int A, int D>
struct TemplateParameters {
  void * __ptrauth(K, 0, 100) m1; // expected-error {{expression is not an integer constant expression}}
  void * __ptrauth(0, A, 100) m2; // expected-error {{argument to '__ptrauth' must be an integer constant expression}}
  void * __ptrauth(0, 0, D) m3; // expected-error {{argument to '__ptrauth' must be an integer constant expression}}
};

void f3() {
  // FIXME: consider loosening the restrictions so that the first two cases are accepted.
  Indirect<void* __ptrauth(0,0,1234)> one;
  // expected-note@-1{{in instantiation of template class 'Indirect<void *__ptrauth(0,0,1234)>' requested here}}
  Indirect<void* __ptrauth(0,0,1235)> two;
  // expected-note@-1{{in instantiation of template class 'Indirect<void *__ptrauth(0,0,1235)>' requested here}}
  Indirect<void*> three;
  Indirect<void* __ptrauth(-1,0,1235)> four;
  // expected-note@-1{{in instantiation of template class 'Indirect<void *__ptrauth(ptrauth_key_none)>' requested here}}
}

#endif

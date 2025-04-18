// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -verify -fptrauth-intrinsics %s
#if __has_extension(ptrauth_restricted_intptr_qualifier)
int *__ptrauth_restricted_intptr(0) a;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'int *' is invalid}}

char __ptrauth_restricted_intptr(0) b;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'char' is invalid}}
unsigned char __ptrauth_restricted_intptr(0) c;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'unsigned char' is invalid}}
short __ptrauth_restricted_intptr(0) d;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'short' is invalid}}
unsigned short __ptrauth_restricted_intptr(0) e;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'unsigned short' is invalid}}
int __ptrauth_restricted_intptr(0) f;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'int' is invalid}}
unsigned int __ptrauth_restricted_intptr(0) g;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'unsigned int' is invalid}}
__int128_t __ptrauth_restricted_intptr(0) h;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; '__int128_t' (aka '__int128') is invalid}}
unsigned short __ptrauth_restricted_intptr(0) i;
// expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'unsigned short' is invalid}}

unsigned long long __ptrauth_restricted_intptr(0) j;
long long __ptrauth_restricted_intptr(0) k;
__SIZE_TYPE__ __ptrauth_restricted_intptr(0) l;
const unsigned long long __ptrauth_restricted_intptr(0) m;
const long long __ptrauth_restricted_intptr(0) n;
const __SIZE_TYPE__ __ptrauth_restricted_intptr(0) o;

struct S1 {
  __SIZE_TYPE__ __ptrauth_restricted_intptr(0) f0;
};
struct S2 {
  int *__ptrauth_restricted_intptr(0) f0;
  // expected-error@-1{{'__ptrauth_restricted_intptr' qualifier only applies to pointer sized integer types; 'int *' is invalid}}
};

void x(unsigned long long __ptrauth_restricted_intptr(0) f0);
// expected-error@-1{{parameter type may not be qualified with '__ptrauth_restricted_intptr'; type is '__ptrauth_restricted_intptr(0,0,0) unsigned long long'}}

unsigned long long __ptrauth_restricted_intptr(0) y();
// expected-error@-1{{return type may not be qualified with '__ptrauth_restricted_intptr'; type is '__ptrauth_restricted_intptr(0,0,0) unsigned long long'}}
#endif
// RUN: %clang_cc1 -E %s -triple=aarch64 -fptrauth-intrinsics | \
// RUN:   FileCheck %s --check-prefixes=INTRIN

// RUN: %clang_cc1 -E %s -triple=aarch64 -fptrauth-calls | \
// RUN:   FileCheck %s --check-prefixes=NOINTRIN

#if __has_extension(ptrauth_qualifier)
void has_ptrauth_qualifier() {}
#else
void no_ptrauth_qualifier() {}
#endif

// INTRIN: has_ptrauth_qualifier
// NOINTRIN: no_ptrauth_qualifier

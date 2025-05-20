// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fbounds-safety -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fbounds-safety -x c++ -fexperimental-bounds-safety-cxx -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fbounds-safety -x objective-c -fexperimental-bounds-safety-objc -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fexperimental-bounds-safety-attributes -x c -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fexperimental-bounds-safety-attributes -x c++ -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fexperimental-bounds-safety-attributes -x objective-c -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-ios -fsyntax-only -fexperimental-bounds-safety-attributes -x objective-c++ -ast-dump %s 2>&1 | FileCheck %s

#include <stddef.h>

typedef __builtin_va_list va_list;

#define __printflike(fmtarg, firstvararg)                                      \
  __attribute__((__format__(__printf__, fmtarg, firstvararg)))

// CHECK: ParmVarDecl {{.+}} foobar 'va_list':'char *'
int vsnprintf(char *__str, size_t __size, const char *__format, va_list foobar)
    __printflike(3, 0);

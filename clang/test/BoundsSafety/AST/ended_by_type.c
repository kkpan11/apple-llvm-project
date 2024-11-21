
// RUN: %clang_cc1 -fbounds-safety -ast-dump %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -fbounds-safety -x objective-c -fbounds-attributes-objc-experimental -ast-dump %s 2>&1 | FileCheck %s

#include <ptrcheck.h>

// CHECK: VarDecl {{.+}} func_ptr_dd 'void (*__single)(void *__single __ended_by(end), void *__single /* __started_by(start) */ )'
void (*func_ptr_dd)(void *__ended_by(end) start, void *end);

// CHECK: VarDecl {{.+}} func_ptr_di 'void (*__single)(void *__single __ended_by(*end), void *__single /* __started_by(start) */ *__single)'
void (*func_ptr_di)(void *__ended_by(*end) start, void **end);

// CHECK: VarDecl {{.+}} func_ptr_id 'void (*__single)(void *__single __ended_by(end)*__single, void *__single /* __started_by(*start) */ )'
void (*func_ptr_id)(void *__ended_by(end) *start, void *end);

// CHECK: VarDecl {{.+}} func_ptr_ii 'void (*__single)(void *__single __ended_by(*end)*__single, void *__single /* __started_by(*start) */ *__single)'
void (*func_ptr_ii)(void *__ended_by(*end) *start, void **end);

void foo(void) {
  // CHECK: CStyleCastExpr {{.+}} 'void (*)(void *__single __ended_by(end), void *__single /* __started_by(start) */ )'
  (void (*)(void *__ended_by(end) start, void *end))0;
}

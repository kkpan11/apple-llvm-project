
// RUN: %clang_cc1 -triple x86_64-apple-mac -ast-dump -fbounds-safety %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-apple-mac -ast-dump -fbounds-safety -x objective-c -fbounds-attributes-objc-experimental %s 2>&1 | FileCheck %s
#include <ptrcheck.h>


// CHECK: RecordDecl {{.*}} struct S definition
// CHECK: |-FieldDecl {{.*}} referenced end 'int *__single /* __started_by(iter) */ ':'int *__single'
// CHECK: |-FieldDecl {{.*}} referenced start 'int *__single __ended_by(iter)':'int *__single'
// CHECK: `-FieldDecl {{.*}} referenced iter 'int *__single __ended_by(end) /* __started_by(start) */ ':'int *__single'
struct S {
    int *end;
    int *__ended_by(iter) start;
    int *__ended_by(end) iter;
};

// CHECK: FunctionDecl {{.*}} foo 'void (int *__single __ended_by(end), int *__single /* __started_by(start) */ )'
// CHECK: |-ParmVarDecl {{.*}} used start 'int *__single __ended_by(end)':'int *__single'
// CHECK: `-ParmVarDecl {{.*}} used end 'int *__single /* __started_by(start) */ ':'int *__single'
void foo(int *__ended_by(end) start, int* end);

// CHECK: FunctionDecl {{.*}} foo_cptr_end 'void (int *__single __ended_by(end), char *__single /* __started_by(start) */ )'
// CHECK: |-ParmVarDecl {{.*}} used start 'int *__single __ended_by(end)':'int *__single'
// CHECK: `-ParmVarDecl {{.*}} used end 'char *__single /* __started_by(start) */ ':'char *__single'
void foo_cptr_end(int *__ended_by(end) start, char* end);

// CHECK: FunctionDecl {{.*}} foo_seq 'void (int *__single __ended_by(next), int *__single __ended_by(end) /* __started_by(start) */ , char *__single /* __started_by(next) */ )'
// CHECK: |-ParmVarDecl {{.*}} used start 'int *__single __ended_by(next)':'int *__single'
// CHECK: |-ParmVarDecl {{.*}} used next 'int *__single __ended_by(end) /* __started_by(start) */ ':'int *__single'
// CHECK: `-ParmVarDecl {{.*}} used end 'char *__single /* __started_by(next) */ ':'char *__single'
void foo_seq(int *__ended_by(next) start, int *__ended_by(end) next, char* end);

// CHECK: FunctionDecl {{.*}} foo_out_start_out_end 'void (int *__single __ended_by(*out_end)*__single, int *__single /* __started_by(*out_start) */ *__single)'
// CHECK: |-ParmVarDecl {{.*}} used out_start 'int *__single __ended_by(*out_end)*__single'
// CHECK: `-ParmVarDecl {{.*}} used out_end 'int *__single /* __started_by(*out_start) */ *__single'
void foo_out_start_out_end(int *__ended_by(*out_end) *out_start, int **out_end);

// CHECK: FunctionDecl {{.*}} foo_out_end 'void (int *__single __ended_by(*out_end), int *__single /* __started_by(start) */ *__single)'
// CHECK: |-ParmVarDecl {{.*}} used start 'int *__single __ended_by(*out_end)':'int *__single'
// CHECK: `-ParmVarDecl {{.*}} used out_end 'int *__single /* __started_by(start) */ *__single'
void foo_out_end(int *__ended_by(*out_end) start, int **out_end);

// CHECK: FunctionDecl {{.*}} foo_ret_end 'int *__single __ended_by(end)(int *__single)'
// CHECK: `-ParmVarDecl {{.*}} used end 'int *__single'
int *__ended_by(end) foo_ret_end(int *end);

// CHECK: FunctionDecl {{.*}} foo_local_ended_by 'void (void)'
// CHECK: `-CompoundStmt
// CHECK:   |-DeclStmt
// CHECK:   | `-VarDecl {{.*}} used end 'int *__single /* __started_by(start) */ ':'int *__single'
// CHECK:   `-DeclStmt
// CHECK:     `-VarDecl {{.*}} used start 'int *__single __ended_by(end)':'int *__single'
void foo_local_ended_by(void) {
  int *end;
  int *__ended_by(end) start;
}

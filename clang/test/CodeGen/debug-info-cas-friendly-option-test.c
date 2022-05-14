// RUN: %clang -cc1 -debug-info-kind=standalone -cas-friendliness-kind=cas-friendly %s -emit-llvm -o - | FileCheck %s
// CHECK: !DICompileUnit{{.+}}casFriendly: true{{.*}}
void foo() {
  return;
}

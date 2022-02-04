// RUN: %clang -cc1 -debug-info-kind=cas-friendly %s -emit-llvm -o - | FileCheck %s
// CHECK: !DICompileUnit{{.+}}emissionKind: CasFriendly{{.*}}
void foo() {
	return;
}

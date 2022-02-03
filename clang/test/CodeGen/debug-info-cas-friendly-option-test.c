// RUN: %clang -cc1 -debug-info-kind=cas-friendly -triple -x86_64-apple-macosx12.0.0 %s -emit-llvm -o - | FileCheck %s
// CHECK: !DICompileUnit{{.+}}emissionKind: CasFriendly{{.*}}
void foo() {
	return;
}

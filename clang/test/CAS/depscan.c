// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -cc1depscan -fdepscan=inline -cc1-args -cc1 -triple x86_64-apple-macos11.0 -x c %s -o %s.o -fcas-path %t/cas 2>&1 | FileCheck %s -DPREFIX=%t
// RUN: %clang -cc1depscan -fdepscan=inline -cc1-args -triple x86_64-apple-macos11.0 -x c %s -o %s.o -fcas-path %t/cas 2>&1 | FileCheck %s -DPREFIX=%t
//
// Check that inline/daemon have identical output.
// RUN: %clang -cc1depscan -o %t/inline.rsp -fdepscan=inline -cc1-args -triple x86_64-apple-macos11.0 -x c %s -o %s.o -fcas-path %t/cas
// RUN: %clang -cc1depscan -o %t/daemon.rsp -fdepscan=daemon -cc1-args -triple x86_64-apple-macos11.0 -x c %s -o %s.o -fcas-path %t/cas
// RUN: diff %t/inline.rsp %t/daemon.rsp

// CHECK: {{^}}"-cc1"
// CHECK: "-fcas-path" "[[PREFIX]]/cas"

int test() { return 0; }

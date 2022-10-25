// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -g -c -fcas-friendly-debug-info=debug-line-only --target=x86_64-apple-darwin21.6.0 -Xclang -fcas-backend -Xclang -fcas-backend-mode=verify -Xclang -fcas-path -Xclang %t/cas %s 
// RUN: %clang -g -c -fcas-friendly-debug-info=debug-line-only --target=x86_64-apple-darwin21.6.0 -Xclang -fcas-backend -Xclang -fcas-backend-mode=casid -Xclang -fcas-path -Xclang %t/cas %s -o %t/cas-file.o
// RUN: llvm-cas-object-format --casid-file %t/cas-file.o --object-stats=- --cas %t/cas | FileCheck %s

//CHECK: mc:debug_line             2 {{[0-9]+}}.{{[0-9]+}}% 2 {{[0-9]+}}.{{[0-9]+}}% 0{{.*}}

int foo() {
    return 1;
}

int bar(int x) {
    return x*x;
}

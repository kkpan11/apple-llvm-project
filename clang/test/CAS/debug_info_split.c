// RUN: %clang -g -c -o %t %s --target=arm64-apple-darwin10 \
// RUN:   -fcas-friendly-debug-info=debug-abbrev -Xclang -fcas-backend \
// RUN:   -Xclang -fcas-backend-mode=verify

int gv1 = 10;

int foo1() {
  return 1;
}

double foo2() {
  return 2;
}

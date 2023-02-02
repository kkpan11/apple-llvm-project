// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s

// TODO test with #include-d file
// TODO test if there's not a single character in the file after a decl or def

void local_array_subscript_simple(int *p);
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-1]]:35-[[@LINE-1]]:40}:"std::span<int> "
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-2]]:42-[[@LINE-2]]:42}:";\n{{..}}clang::unsafe_buffer_usage{{..}} void local_array_subscript_simple(int *p)"

void local_array_subscript_simple(int *a);
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-1]]:35-[[@LINE-1]]:40}:"std::span<int> "
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-2]]:42-[[@LINE-2]]:42}:";\n{{..}}clang::unsafe_buffer_usage{{..}} void local_array_subscript_simple(int *p)"

void local_array_subscript_simple(int *p) {
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-1]]:35-[[@LINE-1]]:40}:"std::span<int> "
  int tmp;
  tmp = p[5];
}
// CHECK-DAG: fix-it:"{{.*}}":{[[@LINE-1]]:2-[[@LINE-1]]:2}:"\n{{..}}clang::unsafe_buffer_usage{{..}} void local_array_subscript_simple(int *p)\n{\n  return local_array_subscript_simple(std::span<int>(p, <# size #>));\n}\n"

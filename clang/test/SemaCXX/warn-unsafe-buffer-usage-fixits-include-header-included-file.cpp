// RUN: %clang_cc1 -std=c++20 -Wunsafe-buffer-usage -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s

// CHECK: fix-it:"{{.*}}warn-unsafe-buffer-usage-fixits-include-header.cpp":{5:1-5:1}:"#include <span>\n"

// The point of this test is to make sure that if we emit Fix-Its for an included file we also add the #include directive to that file (and not necessarily the main file).

#include "warn-unsafe-buffer-usage-fixits-include-header.cpp"

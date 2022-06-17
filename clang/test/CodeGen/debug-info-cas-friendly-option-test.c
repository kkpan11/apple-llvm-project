// RUN: %clang -cc1 -debug-info-kind=standalone -cas-friendliness-kind=debug-line-only %s -emit-llvm -o - | FileCheck %s --check-prefix DEBUG_LINE_ONLY
// DEBUG_LINE_ONLY: !DICompileUnit{{.+}}casFriendly: DebugLineOnly{{.*}}

// RUN: %clang -cc1 -debug-info-kind=standalone -cas-friendliness-kind=debug-abbrev %s -emit-llvm -o - | FileCheck %s --check-prefix DEBUG_ABBREV
// DEBUG_ABBREV: !DICompileUnit{{.+}}casFriendly: DebugAbbrev{{.*}}
void foo() {
  return;
}

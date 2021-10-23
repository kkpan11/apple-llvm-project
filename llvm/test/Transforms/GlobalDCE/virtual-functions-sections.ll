; RUN: opt < %s -globaldce -lowertypetests -lowertypetests-summary-action=export -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

declare { i8*, i1 } @llvm.type.checked.load(i8*, i32, metadata)

@vtable = internal unnamed_addr constant { [2 x i8*] } { [2 x i8*] [
  i8* bitcast (void ()* @vfunc1_live to i8*),
  i8* bitcast (void ()* @vfunc2_dead to i8*)
]}, align 8, !type !0, !type !1, !vcall_visibility !{i64 2}, !vtable_strict_types !998, section "my_custom_section"
!0 = !{i64 0, !"vfunc1.type"}
!1 = !{i64 8, !"vfunc2.type"}

define internal void @vfunc1_live() {
  ; CHECK: define internal void @vfunc1_live(
  ret void
}

define internal void @vfunc2_dead() {
  ; CHECK-NOT: define internal void @vfunc2_dead(
  ret void
}

define void @main() {
  %1 = ptrtoint { [2 x i8*] }* @vtable to i64 ; to keep @vtable alive
  %2 = tail call { i8*, i1 } @llvm.type.checked.load(i8* null, i32 0, metadata !"vfunc1.type")
  ret void
}

!998 = !{}
!999 = !{i32 1, !"Virtual Function Elim", i32 1}
!llvm.module.flags = !{!999}

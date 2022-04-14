; RUN: opt -objc-arc-contract -S < %s | FileCheck %s
; RUN: opt -passes=objc-arc-contract -S < %s | FileCheck %s

; CHECK-LABEL: define void @test0() {
; CHECK: %[[CALL:.*]] = notail call i8* @foo() [ "clang.arc.attachedcall"() ]
; CHECK-NEXT: call void asm sideeffect "mov\09fp, fp\09\09// marker for objc_retainAutoreleaseReturnValue", ""()
; CHECK-NEXT: call i8* @llvm.objc.retainAutoreleasedReturnValue(i8* %[[CALL]])

define void @test0() {
  %call1 = notail call i8* @foo() [ "clang.arc.attachedcall"() ]
  call i8* @llvm.objc.retainAutoreleasedReturnValue(i8* %call1)
  ret void
}

; CHECK-LABEL: define void @test1() {
; CHECK: %[[CALL:.*]] = notail call i8* @foo() [ "clang.arc.attachedcall"() ]
; CHECK-NEXT: call void asm sideeffect "mov\09fp, fp\09\09// marker for objc_retainAutoreleaseReturnValue", ""()
; CHECK-NEXT: call i8* @llvm.objc.unsafeClaimAutoreleasedReturnValue(i8* %[[CALL]])

define void @test1() {
  %call1 = notail call i8* @foo() [ "clang.arc.attachedcall"() ]
  call i8* @llvm.objc.unsafeClaimAutoreleasedReturnValue(i8* %call1)
  ret void
}

declare i8* @foo()
declare i8* @llvm.objc.retainAutoreleasedReturnValue(i8*)
declare i8* @llvm.objc.unsafeClaimAutoreleasedReturnValue(i8*)

!llvm.module.flags = !{!0}

!0 = !{i32 1, !"clang.arc.retainAutoreleasedReturnValueMarker", !"mov\09fp, fp\09\09// marker for objc_retainAutoreleaseReturnValue"}

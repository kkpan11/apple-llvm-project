; RUN: opt --passes=deadargelim %s -S -o - | FileCheck %s

define internal fastcc i8* @bar() {
; CHECK: define internal fastcc i8* @bar
  ret i8* null
}

define void @foo() {
; CHECK: tail call fastcc i8* @bar()
  tail call fastcc i8* @bar() [ "clang.arc.attachedcall"(i8* (i8*)* @llvm.objc.retainAutoreleasedReturnValue) ]
  ret void
}

declare i8* @llvm.objc.retainAutoreleasedReturnValue(i8*)

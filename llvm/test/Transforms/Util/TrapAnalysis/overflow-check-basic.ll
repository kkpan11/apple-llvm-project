; A trap edge whose guard is the overflow bit of a checked-arithmetic intrinsic
; (extractvalue(llvm.{s,u}{add,sub,mul}.with.overflow, 1)) is classified as
; OverflowCheck rather than an index/bounds comparison; a plain icmp-guarded edge
; on the same loop shape keeps its normal class. Gated by
; -loop-trap-analysis-explain.

; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

declare void @llvm.trap()
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)

; The in-loop trap exit is guarded by the overflow bit of a checked add ->
; OverflowCheck.
define void @overflow_inc(i64 %n) {
entry:
  br label %body
body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %ov = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %iv, i64 1)
  %bit = extractvalue { i64, i1 } %ov, 1
  br i1 %bit, label %trap, label %latch
trap:
  call void @llvm.trap()
  unreachable
latch:
  %iv.next = extractvalue { i64, i1 } %ov, 0
  %done = icmp eq i64 %iv.next, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}overflow_inc
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - TrapBB:{{ +}}trap
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}OverflowCheck
; CHECK:        - HasOverflowBitLeaf:{{ +}}'true'

; The same loop shape guarded by a plain bounds icmp is not reclassified.
define void @bounds_check(ptr %base, i64 %n) {
entry:
  br label %body
body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %cmp = icmp ult i64 %iv, %n
  br i1 %cmp, label %latch, label %trap
trap:
  call void @llvm.trap()
  unreachable
latch:
  %iv.next = add nuw nsw i64 %iv, 1
  %e = icmp eq i64 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Function:{{ +}}bounds_check
; CHECK:        - TrapBB:{{ +}}trap
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountKnown
; CHECK:        - HasOverflowBitLeaf:{{ +}}'false'

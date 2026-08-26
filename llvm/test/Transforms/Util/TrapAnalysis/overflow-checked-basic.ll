; A NotProvenMonotonic trap edge whose index IV is derived from the VALUE result
; of a checked-arithmetic intrinsic (extractvalue(llvm.sadd.with.overflow, 0),
; i.e. Swift's checked `+`) gets an -OverflowChecked suffix, marking that SCEV's
; missing no-wrap could be recovered by proving the checked add cannot overflow
; (e.g. by IndVars after hoisting). A plain (unchecked) IV of the same shape
; keeps its class. Gated by -loop-trap-analysis-explain.

; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

declare void @llvm.trap()
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)

; The IV is advanced by a checked add with a variable step, so the AddRec is
; affine but SCEV cannot prove no-wrap / a trip count -> NotProvenMonotonic, and
; the index traces to extractvalue(sadd.with.overflow, 0) -> OverflowChecked.
define void @checked_step(ptr %base, i64 %n, i64 %step) {
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
  %ov = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %iv, i64 %step)
  %iv.next = extractvalue { i64, i1 } %ov, 0
  %e = icmp eq i64 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}checked_step
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride-OverflowChecked

; The same loop shape with a plain (unchecked) variable-step add is NOT tagged.
define void @plain_step(ptr %base, i64 %n, i64 %step) {
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
  %iv.next = add i64 %iv, %step
  %e = icmp eq i64 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Function:{{ +}}plain_step
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride

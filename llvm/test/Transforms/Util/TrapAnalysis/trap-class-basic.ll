; Per-edge LoopTrapEdge remarks carry a TrapClass field: a count-ordered,
; descriptive classification whose leading token is the index-shape axis
; (Invariant- / Affine- / Opaque-). This test covers the cases that classify
; without reload / alias / opaque-operand inputs (those light up in a later
; slice); here they are 0, so the non-blocked Invariant-/Affine- classes are
; emitted. The field is gated by -loop-trap-analysis-explain.

; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

declare void @llvm.trap()

; A trap check outside any loop sitting directly on the function entry block ->
; Invariant-OutsideLoop-EntryProximate.
define void @entry_proximate(i32 %n) {
entry:
  %c = icmp slt i32 %n, 0
  br i1 %c, label %trap, label %cont
trap:
  call void @llvm.trap()
  unreachable
cont:
  br label %body
body:
  %iv = phi i32 [ 0, %cont ], [ %iv.next, %body ]
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}entry_proximate
; CHECK:        - SourceBB:{{ +}}entry
; CHECK:        - IsLoopExit:{{ +}}'false'
; CHECK:        - TrapClass:{{ +}}Invariant-OutsideLoop-EntryProximate

; A single-comparison trap check outside any loop, reached only after a
; straight-line chain of blocks longer than the entry-proximity depth ->
; Invariant-OutsideLoop-SingleComparison.
define void @out_single_cmp(i32 %n, i32 %x) {
entry:
  br label %d1
d1:
  %a = add i32 %x, 1
  br label %d2
d2:
  %b = add i32 %a, 1
  br label %d3
d3:
  %cc = add i32 %b, 1
  br label %d4
d4:
  %dd = add i32 %cc, 1
  br label %chk
chk:
  %c = icmp slt i32 %dd, 0
  br i1 %c, label %trap, label %cont
trap:
  call void @llvm.trap()
  unreachable
cont:
  br label %body
body:
  %iv = phi i32 [ 0, %cont ], [ %iv.next, %body ]
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}out_single_cmp
; CHECK:        - SourceBB:{{ +}}chk
; CHECK:        - IsLoopExit:{{ +}}'false'
; CHECK:        - TrapClass:{{ +}}Invariant-OutsideLoop-SingleComparison

; A counted loop whose in-loop trap exit has a computable per-edge trip count ->
; Affine-InLoopExit-TripCountKnown.
define void @counted_trap(ptr %base, i32 %n) {
entry:
  %c0 = icmp eq i32 %n, 0
  br i1 %c0, label %exit, label %body
body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %cmp = icmp ult i32 %iv, %n
  br i1 %cmp, label %latch, label %trap
trap:
  call void @llvm.trap()
  unreachable
latch:
  %p = getelementptr i32, ptr %base, i32 %iv
  store i32 0, ptr %p, align 4
  %iv.next = add nuw nsw i32 %iv, 1
  %e = icmp eq i32 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}counted_trap
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountKnown

; A trap exit driven by a pointer IV with a non-unit constant stride (i8, +2),
; so the per-edge trip count is not computable and the AddRec is not proven
; monotonic -> Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonUnitStride.
define void @stride_nonunit(ptr %base, ptr %bound) {
entry:
  br label %body
body:
  %iv = phi ptr [ %base, %entry ], [ %iv.next, %latch ]
  %cmp = icmp ult ptr %iv, %bound
  br i1 %cmp, label %latch, label %trap
trap:
  call void @llvm.trap()
  unreachable
latch:
  %iv.next = getelementptr i8, ptr %iv, i32 2
  %l = load i8, ptr %iv
  %e = icmp eq i8 %l, 0
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}stride_nonunit
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonUnitStride

; A trap exit driven by a pointer IV whose stride is a runtime (non-constant)
; value -> Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride.
define void @stride_variable(ptr %base, ptr %bound, i32 %s) {
entry:
  br label %body
body:
  %iv = phi ptr [ %base, %entry ], [ %iv.next, %latch ]
  %cmp = icmp ult ptr %iv, %bound
  br i1 %cmp, label %latch, label %trap
trap:
  call void @llvm.trap()
  unreachable
latch:
  %iv.next = getelementptr i32, ptr %iv, i32 %s
  %l = load i32, ptr %iv
  %e = icmp eq i32 %l, 0
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}stride_variable
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride

; An in-loop trap exit on an accumulator (%acc += %iv) whose AddRec carries only
; a weak no-wrap flag (not proven monotonic) and has a unit stride ->
; Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic.
define void @not_proven_monotonic(i32 %n) {
entry:
  br label %body
body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %latch ]
  %cmp = icmp slt i32 %acc, %n
  br i1 %cmp, label %latch, label %trap
trap:
  call void @llvm.trap()
  unreachable
latch:
  %acc.next = add i32 %acc, %iv
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, 100
  br i1 %e, label %exit, label %body
exit:
  ret void
}
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}not_proven_monotonic
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic

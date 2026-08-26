; Per-edge LoopTrapEdge remarks carry SCEV-descriptive "explain" fields beyond
; TrapClass: whether every predicate operand had a computable SCEV, whether the
; predicate is loop-invariant, whether it contains an AddRec, the per-edge trip
; count computability, and (under -loop-trap-analysis-explain) whether the trap
; branch and the IV update dominate the loop latch. This test covers an affine
; IV trap-exit where those fields take their canonical values.

; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

declare void @llvm.trap()

; A counted loop whose in-loop check (icmp ult iv, n) branches to an
; @llvm.trap+unreachable block. The trapping index is an affine AddRec over the
; loop IV, so: SCEVComputed=true, HasAddRec=true, EdgeBTCComputable=true. The
; branch dominates the latch and the IV update is unconditional, so both
; DominatesLatch and IVUpdateDominatesLatch are true.
define void @affine_trap(ptr %base, i32 %n) {
entry:
  br label %body
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
; CHECK:      Function:{{ +}}affine_trap
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - TrapBB:{{ +}}trap
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountKnown
; CHECK:        - NumLeafOperands:{{ +}}'2'
; CHECK:        - SCEVComputed:{{ +}}'true'
; CHECK:        - SCEVLoopInvariant:{{ +}}'false'
; CHECK:        - HasAddRec:{{ +}}'true'
; CHECK:        - HasInLoopUnknown:{{ +}}'false'
; CHECK:        - HasNonUnitStrideForLAddRec:{{ +}}'false'
; CHECK:        - EdgeBTCComputable:{{ +}}'true'
; CHECK:        - EdgeBTCSymbolic:{{ +}}'false'
; CHECK:        - DominatesLatch:{{ +}}'true'
; CHECK:        - IVUpdateDominatesLatch:{{ +}}'true'

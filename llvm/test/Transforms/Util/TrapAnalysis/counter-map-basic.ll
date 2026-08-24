; A single innermost loop with two trap exits: one whose per-edge SCEV trip
; count is computable (icmp ult %iv, %n -> trap1) and one whose predicate
; reloads an in-loop store (icmp slt %v, 0 -> trap2), leaving that exit's BTC
; uncomputable. The uncomputable sibling makes UncomputableTrapExits[L] > 0, so
; the computable edge classifies as
; Affine-InLoopExit-TripCountKnown-LoopBlockedOtherTrapExit and the per-loop
; LoopPrimitives record carries the unknown-BTC breakdown counters (stored in a
; tag-keyed map) nonzero. All fields gated by -loop-trap-analysis-explain.

; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

declare void @llvm.trap()

define void @two_trap_exits(ptr %base, i32 %n) {
entry:
  br label %body
body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %cmp = icmp ult i32 %iv, %n
  br i1 %cmp, label %chk2, label %trap1
trap1:
  call void @llvm.trap()
  unreachable
chk2:
  %p = getelementptr i32, ptr %base, i32 %iv
  %v = load i32, ptr %p, align 4
  %bad = icmp slt i32 %v, 0
  br i1 %bad, label %trap2, label %latch
trap2:
  call void @llvm.trap()
  unreachable
latch:
  store i32 0, ptr %p, align 4
  %iv.next = add nuw nsw i32 %iv, 1
  %e = icmp eq i32 %iv.next, %n
  br i1 %e, label %exit, label %body
exit:
  ret void
}

; The per-loop LoopPrimitives record surfaces the unknown-BTC counter map: the
; store-reload exit is counted once in trap_exits_unknown_btc (subclassed as a
; store-reload blocker), and the computable sibling in trap_exits_computable_btc.
; CHECK:      Name:{{ +}}LoopPrimitives
; CHECK:      Function:{{ +}}two_trap_exits
; CHECK:        - LoopHeader:{{ +}}body
; CHECK:        - TrapExitCount:{{ +}}'2'
; CHECK:        - TrapExitsUnknownBTC:{{ +}}'1'
; CHECK:        - TrapExitsUnknownBTCDueToReload:{{ +}}'1'
; CHECK:        - TrapExitsUnknownBTCStoreReload:{{ +}}'1'
; CHECK:        - TrapExitsComputableBTC:{{ +}}'1'
; CHECK:        - InvocationSeq:{{ +}}'1'

; The computable trap edge (source BB body) is masked by the loop's other
; uncomputable trap exit -> LoopBlockedOtherTrapExit variant.
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}two_trap_exits
; CHECK:        - SourceBB:{{ +}}body
; CHECK:        - IsLoopExit:{{ +}}'true'
; CHECK:        - TrapClass:{{ +}}Affine-InLoopExit-TripCountKnown-LoopBlockedOtherTrapExit
; CHECK:        - EdgeBTCComputable:{{ +}}'true'
; CHECK:        - LoopHasOtherUnknownBTCTrap:{{ +}}'true'

; The store-reload trap edge (source BB chk2) is the uncomputable sibling.
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK:      Function:{{ +}}two_trap_exits
; CHECK:        - SourceBB:{{ +}}chk2
; CHECK:        - TrapClass:{{ +}}Opaque-InLoopExit-TripCountUnknown-StoreReload
; CHECK:        - EdgeBTCComputable:{{ +}}'false'
; CHECK:        - LoopHasOtherUnknownBTCTrap:{{ +}}'false'

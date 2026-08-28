; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s

; Exercises the reload/alias + opaque-operand operand classification: the
; Opaque-* TrapClass classes and their operand NV fields (HasStoreReload,
; HasInLoopPhiOperand, HasOtherInLoopUnknownOperand, ...). All three loops
; have a non-computable per-edge trip count (EdgeBTCComputable=false), so the
; blocker classes are what selects the TrapClass.

declare void @llvm.trap()
declare i32 @opaque()

; In-loop trap exit whose condition reloads %p, which a same-loop store to %q
; could alias and modify -> Opaque-...-StoreReload.
define void @store_reload(ptr %p, ptr %q) {
entry:
  br label %body

body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %v = load i32, ptr %p
  %cmp = icmp ult i32 %iv, %v
  br i1 %cmp, label %latch, label %trap

trap:
  call void @llvm.trap()
  unreachable

latch:
  store i32 %iv, ptr %q
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, 100
  br i1 %e, label %exit, label %body

exit:
  ret void
}
; CHECK: Function:{{ +}}store_reload
; CHECK: TrapClass:{{ +}}Opaque-InLoopExit-TripCountUnknown-StoreReload
; CHECK: HasStoreReload:{{ +}}'true'
; CHECK: ReloadStoreKind:{{ +}}store
; CHECK: ReloadLoadName:{{ +}}p
; CHECK: HasCallReload:{{ +}}'false'
; CHECK: HasOtherInLoopUnknownOperand:{{ +}}'false'

; In-loop trap exit whose condition operand is the result of @opaque(), which
; has no characterizable SCEV -> Opaque-...-OpaqueOperand-Other.
define void @opaque_other(i32 %n) {
entry:
  br label %body

body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %o = call i32 @opaque()
  %cmp = icmp slt i32 %o, %n
  br i1 %cmp, label %latch, label %trap

trap:
  call void @llvm.trap()
  unreachable

latch:
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, 100
  br i1 %e, label %exit, label %body

exit:
  ret void
}
; CHECK: Function:{{ +}}opaque_other
; CHECK: TrapClass:{{ +}}Opaque-InLoopExit-TripCountUnknown-OpaqueOperand-Other
; CHECK: HasStoreReload:{{ +}}'false'
; CHECK: HasOtherInLoopUnknownOperand:{{ +}}'true'

; In-loop trap exit whose condition depends on %acc, a non-IV phi updated only
; on some iterations (via a select) -> Opaque-...-InLoopPhiOperand.
define void @in_loop_phi_operand(i32 %n, ptr %p) {
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
  %ld = load i32, ptr %p
  %pos = icmp sgt i32 %ld, 0
  %acc.inc = add i32 %acc, 1
  %acc.next = select i1 %pos, i32 %acc.inc, i32 %acc
  %iv.next = add i32 %iv, 1
  %e = icmp eq i32 %iv.next, %n
  br i1 %e, label %exit, label %body

exit:
  ret void
}
; CHECK: Function:{{ +}}in_loop_phi_operand
; CHECK: TrapClass:{{ +}}Opaque-InLoopExit-TripCountUnknown-InLoopPhiOperand
; CHECK: HasInLoopPhiOperand:{{ +}}'true'

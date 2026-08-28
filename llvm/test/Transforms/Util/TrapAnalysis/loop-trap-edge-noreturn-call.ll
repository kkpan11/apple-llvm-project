; RUN: opt -passes='loop-trap-analysis' -loop-trap-analysis-explain -disable-output \
; RUN:   -pass-remarks-output=%t.yaml %s
; RUN: FileCheck --input-file=%t.yaml %s \
; RUN:   --implicit-check-not=LoopTrapEdge --implicit-check-not=retbb \
; RUN:   --implicit-check-not=sideeffect

declare void @fatal() noreturn
declare void @sideeffect()

; A non-intrinsic `noreturn` call before `unreachable` (%fatalbb, e.g. a
; fatal-error / abort function) is a trap edge on doesNotReturn alone -- it need
; not be an intrinsic and need not only touch inaccessible memory. A plain
; returning call before `unreachable` (%retbb) is still not a trap edge, so
; exactly one LoopTrapEdge is emitted (enforced by --implicit-check-not above).
define void @noreturn_call(ptr %base, i32 %n) {
entry:
  br label %body

body:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %c0 = icmp ult i32 %iv, %n
  br i1 %c0, label %s1, label %fatalbb

fatalbb:
  call void @fatal()
  unreachable

s1:
  %c1 = icmp eq i32 %iv, 3
  br i1 %c1, label %latch, label %retbb

retbb:
  call void @sideeffect()
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

; The single LoopTrapEdge record, pinned line-by-line; it targets %fatalbb.
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}noreturn_call
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}noreturn_call
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}fatalbb
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'1'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Affine-InLoopExit-TripCountKnown
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT: ...

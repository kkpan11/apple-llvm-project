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

; Full LoopTrapEdge record(s), pinned line-by-line.
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}entry_proximate
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}entry_proximate
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}entry
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Invariant-OutsideLoop-EntryProximate
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...
;
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}out_single_cmp
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}out_single_cmp
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}chk
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Invariant-OutsideLoop-SingleComparison
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...
;
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}counted_trap
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}counted_trap
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
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
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...
;
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}stride_nonunit
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}stride_nonunit
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'1'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonUnitStride
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...
;
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}stride_variable
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}stride_variable
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'1'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...
;
; CHECK:      Name:{{ +}}LoopTrapEdge
; CHECK-NEXT: Function:{{ +}}not_proven_monotonic
; CHECK-NEXT: Args:
; CHECK-NEXT:   - String:{{ +}}'Function '
; CHECK-NEXT:   - Function:{{ +}}not_proven_monotonic
; CHECK-NEXT:   - String:{{ +}}' src_bb='
; CHECK-NEXT:   - SourceBB:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' trap_bb='
; CHECK-NEXT:   - TrapBB:{{ +}}trap
; CHECK-NEXT:   - String:{{ +}}' loop_depth='
; CHECK-NEXT:   - LoopDepth:{{ +}}'1'
; CHECK-NEXT:   - String:{{ +}}' loop_header='
; CHECK-NEXT:   - LoopHeader:{{ +}}body
; CHECK-NEXT:   - String:{{ +}}' is_innermost='
; CHECK-NEXT:   - IsInnermost:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' is_loop_exit='
; CHECK-NEXT:   - IsLoopExit:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' trap_class='
; CHECK-NEXT:   - TrapClass:{{ +}}Affine-InLoopExit-TripCountUnknown-NotProvenMonotonic
; CHECK-NEXT:   - String:{{ +}}' num_leaf_operands='
; CHECK-NEXT:   - NumLeafOperands:{{ +}}'2'
; CHECK-NEXT:   - String:{{ +}}' scev_computed='
; CHECK-NEXT:   - SCEVComputed:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' scev_loop_invariant='
; CHECK-NEXT:   - SCEVLoopInvariant:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_addrec='
; CHECK-NEXT:   - HasAddRec:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_unknown='
; CHECK-NEXT:   - HasInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_unit_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonUnitStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_non_constant_stride_for_l_addrec='
; CHECK-NEXT:   - HasNonConstantStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_only_weak_no_wrap_for_l_addrec='
; CHECK-NEXT:   - HasOnlyNotProvenMonotonicForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_negative_stride_for_l_addrec='
; CHECK-NEXT:   - HasNegativeStrideForLAddRec:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_computable='
; CHECK-NEXT:   - EdgeBTCComputable:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' edge_btc_symbolic='
; CHECK-NEXT:   - EdgeBTCSymbolic:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' loop_has_other_unknown_btc_trap='
; CHECK-NEXT:   - LoopHasOtherUnknownBTCTrap:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' predicate_shape='
; CHECK-NEXT:   - PredicateShape:{{ +}}SingleICmp
; CHECK-NEXT:   - String:{{ +}}' is_entry_proximate='
; CHECK-NEXT:   - IsEntryProximate:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' dominated_by_equivalent_check='
; CHECK-NEXT:   - DominatedByEquivalentCheck:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' dominates_latch='
; CHECK-NEXT:   - DominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' iv_update_dominates_latch='
; CHECK-NEXT:   - IVUpdateDominatesLatch:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' loop_latch_btc_computable='
; CHECK-NEXT:   - LoopLatchBTCComputable:{{ +}}'true'
; CHECK-NEXT:   - String:{{ +}}' has_store_reload='
; CHECK-NEXT:   - HasStoreReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_mem_intrinsic_reload='
; CHECK-NEXT:   - HasMemIntrinsicReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' reload_store_line='
; CHECK-NEXT:   - ReloadStoreLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' reload_store_kind='
; CHECK-NEXT:   - ReloadStoreKind:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_store_tbaa='
; CHECK-NEXT:   - ReloadStoreTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_tbaa='
; CHECK-NEXT:   - ReloadLoadTBAA:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_name='
; CHECK-NEXT:   - ReloadLoadName:{{ +}}''
; CHECK-NEXT:   - String:{{ +}}' reload_load_line='
; CHECK-NEXT:   - ReloadLoadLine:{{ +}}'0'
; CHECK-NEXT:   - String:{{ +}}' has_call_reload='
; CHECK-NEXT:   - HasCallReload:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_unaliased_load_operand='
; CHECK-NEXT:   - HasUnaliasedLoadOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_phi_operand='
; CHECK-NEXT:   - HasInLoopPhiOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_freeze_operand='
; CHECK-NEXT:   - HasInLoopFreezeOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_in_loop_select_operand='
; CHECK-NEXT:   - HasInLoopSelectOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_other_in_loop_unknown_operand='
; CHECK-NEXT:   - HasOtherInLoopUnknownOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_opaque_operand_no_in_loop_unknown='
; CHECK-NEXT:   - HasOpaqueOperandNoInLoopUnknown:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_outer_loop_addrec_operand='
; CHECK-NEXT:   - HasOuterLoopAddRecOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_overflow_bit_leaf='
; CHECK-NEXT:   - HasOverflowBitLeaf:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' has_checked_arith_value_operand='
; CHECK-NEXT:   - HasCheckedArithValueOperand:{{ +}}'false'
; CHECK-NEXT:   - String:{{ +}}' invocation_seq='
; CHECK-NEXT:   - InvocationSeq:{{ +}}'1'
; CHECK-NEXT: ...

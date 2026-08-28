//===-- LoopTrapAnalysis.cpp - Loop Trap Count pass -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// LoopTrapAnalysis emits machine-readable opt-remark records describing each
// conditional branch to a trap block (a bounds/overflow check lowered to
// br+@llvm.trap). Under -loop-trap-analysis-explain, each LoopTrapEdge record
// carries the edge's properties along orthogonal axes so a consumer can
// classify it from the fields alone:
//   - condition nature      SCEVLoopInvariant / HasAddRec / HasInLoopUnknown
//   - position              LoopHeader, LoopDepth, IsLoopExit, IsInnermost,
//                           IsEntryProximate
//   - loop trip count       LoopHasOtherUnknownBTCTrap, LoopLatchBTCComputable
//   - condition trip count  EdgeBTCComputable, EdgeBTCSymbolic
//   - condition property    DominatesLatch, IVUpdateDominatesLatch,
//                           DominatedByEquivalentCheck, HasOverflowBitLeaf,
//                           HasCheckedArithValueOperand
//   - operand / stride      Has*Reload, Has*Operand, Has*StrideForLAddRec
//   - comparison shape      PredicateShape, NumLeafOperands
// Each patch in this series fills in one axis. This first patch establishes the
// per-edge record behind the flag; later patches add the fields above.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopTrapAnalysis.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Remarks/BoundsSafetyOptRemarks.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace llvm::ore;
#define DEBUG_TYPE "loop-trap-analysis"
#define REMARK_PASS DEBUG_TYPE

enum class CheckLoopHoistType { MAYBE_CAN_HOIST, CANNOT_HOIST, SKIP };
static cl::opt<bool> NewTrapSemantics(
    "use-new-trap-semantics", cl::init(false),
    cl::desc("Assume that traps are using the new trap semantics "
             "logic."));
static cl::opt<bool> BoundsSafetyTrapsOnly(
    "use-bounds-safety-traps-only", cl::init(false),
    cl::desc(
        "We only check for -fbounds-safety traps if the flag is false we can check "
        "for any hoistable traps."));
static cl::opt<bool> LTAEmitExplain(
    "loop-trap-analysis-explain", cl::init(false),
    cl::desc("Emit the per-trap-edge explain analysis: one LoopTrapEdge remark "
             "per conditional branch to a trap block. Off by default, so the "
             "base remark output is unchanged."));
static cl::opt<unsigned> EntryProximityDepth(
    "loop-trap-entry-proximity-depth", cl::init(3),
    cl::desc("Maximum dominator depth from function entry at which a "
             "trap edge is classified IsEntryProximate (default 3)."));

/// Print a stable, non-empty label for \p BB, so remark args that identify
/// BasicBlocks stay useful when the BB has no source-level name (numeric IR,
/// stripped names, or non-C frontends such as swiftc's IRGen).
///
/// Preferred: `BB->getName()`. Otherwise `printAsOperand` emits the
/// slot-tracker form (`%5` etc.), which is parseable and unique within the
/// function; for any block in a function it yields a `%N` slot, so it is
/// never empty.
static std::string bbLabel(const BasicBlock *BB) {
  if (BB->hasName())
    return BB->getName().str();
  std::string S;
  raw_string_ostream OS(S);
  BB->printAsOperand(OS, /*PrintType=*/false);
  return S;
}

/// Minimal trap-block predicate for the per-edge explain output: \p BB ends in
/// `unreachable` immediately preceded by a trap-like call. @llvm.trap /
/// @llvm.ubsantrap are `noreturn` and only access inaccessible memory, so an
/// intrinsic must have both; a non-intrinsic call qualifies on `noreturn`.
static bool isTrapEdgeBlock(BasicBlock *BB) {
  Instruction *Term = BB->getTerminator();
  if (!isa<UnreachableInst>(Term))
    return false;
  // A bare `unreachable` with no preceding instruction is valid IR; this guards
  // the getPrevNode() below.
  if (Term == &BB->front())
    return false;
  auto *CI = dyn_cast<CallInst>(Term->getPrevNode());
  if (!CI)
    return false;
  // Trap intrinsic: noreturn and only touches inaccessible memory.
  if (isa<IntrinsicInst>(CI))
    return CI->doesNotReturn() && CI->onlyAccessesInaccessibleMemory();
  // Non-intrinsic call: noreturn is enough.
  return CI->doesNotReturn();
}

/// Count loop-exit edges of \p L whose successor is a trap block (see
/// isTrapEdgeBlock).
///
/// Strict attribution: count a trap exit only if it has a predecessor whose
/// immediate containing loop is L (not a nested sub-loop). Otherwise an inner
/// trap's unreachable target -- present in every enclosing loop's exit set --
/// would be counted at every nesting level.
static unsigned countTrapExits(Loop *L, LoopInfo &LI) {
  unsigned Count = 0;
  SmallVector<BasicBlock *, 4> LoopExitBlocks;
  L->getExitBlocks(LoopExitBlocks);
  for (auto *BB : LoopExitBlocks) {
    if (!isTrapEdgeBlock(BB))
      continue;
    if (any_of(predecessors(BB), [L, &LI](BasicBlock *PredB) {
          if (LI.getLoopFor(PredB) != L)
            return false;
          auto *TerminatorInst = PredB->getTerminator();
          return L->contains(PredB) && (isa<CondBrInst>(TerminatorInst) ||
                                        isa<UncondBrInst>(TerminatorInst) ||
                                        isa<SwitchInst>(TerminatorInst));
        }))
      ++Count;
  }
  return Count;
}

/// Emit one LoopPrimitives remark per loop with a trap exit (trap-free loops
/// emit none), plus a per-function LoopPrimitivesSummary with loop-depth
/// tallies over all loops. Gated by -loop-trap-analysis-explain.
static void emitLoopPrimitives(Function &F, LoopInfo &LI,
                               OptimizationRemarkEmitter &ORE,
                               ScalarEvolution &SE) {
  unsigned TotalLoops = 0;
  unsigned Innermost = 0;
  unsigned MaxDepth = 0;
  unsigned Depth1 = 0, Depth2 = 0, Depth3Plus = 0;

  std::string PrimName = "LoopPrimitives";
  std::string SumName = "LoopPrimitivesSummary";

  for (auto *L : LI.getLoopsInPreorder()) {
    ++TotalLoops;
    bool IsInnermost = L->isInnermost();
    if (IsInnermost)
      ++Innermost;
    unsigned Depth = L->getLoopDepth();
    if (Depth > MaxDepth)
      MaxDepth = Depth;
    if (Depth == 1)
      ++Depth1;
    else if (Depth == 2)
      ++Depth2;
    else if (Depth >= 3)
      ++Depth3Plus;

    unsigned TrapExits = countTrapExits(L, LI);
    // loop-trap-analysis: only loops with a trap exit get a record (trap-free
    // loops still count toward the summary below).
    if (TrapExits == 0)
      continue;
    bool BTCKnown = !isa<SCEVCouldNotCompute>(SE.getBackedgeTakenCount(L));

    std::string ParentHeader = "-";
    if (Loop *Parent = L->getParentLoop())
      ParentHeader = bbLabel(Parent->getHeader());

    OptimizationRemarkAnalysis Rem(REMARK_PASS, PrimName,
                                   &L->getHeader()->front());
    Rem << "Loop " << NV("LoopHeader", bbLabel(L->getHeader()))
        << " depth=" << NV("Depth", Depth)
        << " parent=" << NV("ParentHeader", ParentHeader)
        << " innermost=" << NV("IsInnermost", IsInnermost)
        << " blocks=" << NV("BlockCount", (unsigned)L->getNumBlocks())
        << " trap_exits=" << NV("TrapExitCount", TrapExits)
        << " btc_known=" << NV("BTCKnown", BTCKnown);
    ORE.emit(Rem);
  }

  OptimizationRemarkAnalysis Sum(REMARK_PASS, SumName, &F);
  Sum << "Function " << NV("Function", F.getName())
      << " total_loops=" << NV("TotalLoops", TotalLoops)
      << " innermost=" << NV("Innermost", Innermost)
      << " max_depth=" << NV("MaxDepth", MaxDepth)
      << " depth1=" << NV("Depth1", Depth1)
      << " depth2=" << NV("Depth2", Depth2)
      << " depth3+=" << NV("Depth3Plus", Depth3Plus);
  ORE.emit(Sum);
}

/// Recursively unfold a boolean expression chain (select-OR, select-AND,
/// or/and) into the leaf comparison operands. Used to surface every
/// concrete operand of a trap predicate so we can classify each one.
static void collectBoolLeafOperands(Value *V,
                                    SmallVectorImpl<Value *> &Operands,
                                    SmallPtrSetImpl<Value *> &Visited,
                                    int Depth = 0) {
  if (!V || !Visited.insert(V).second || Depth > 8)
    return;
  if (auto *SI = dyn_cast<SelectInst>(V)) {
    Value *T = SI->getTrueValue(), *FV = SI->getFalseValue();
    if (auto *TC = dyn_cast<ConstantInt>(T))
      if (TC->isOne()) {
        collectBoolLeafOperands(SI->getCondition(), Operands, Visited,
                                Depth + 1);
        collectBoolLeafOperands(FV, Operands, Visited, Depth + 1);
        return;
      }
    if (auto *FC = dyn_cast<ConstantInt>(FV))
      if (FC->isZero()) {
        collectBoolLeafOperands(SI->getCondition(), Operands, Visited,
                                Depth + 1);
        collectBoolLeafOperands(T, Operands, Visited, Depth + 1);
        return;
      }
  }
  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    if (BO->getOpcode() == Instruction::Or ||
        BO->getOpcode() == Instruction::And) {
      collectBoolLeafOperands(BO->getOperand(0), Operands, Visited, Depth + 1);
      collectBoolLeafOperands(BO->getOperand(1), Operands, Visited, Depth + 1);
      return;
    }
  }
  if (auto *Cmp = dyn_cast<CmpInst>(V)) {
    Operands.push_back(Cmp->getOperand(0));
    Operands.push_back(Cmp->getOperand(1));
    return;
  }
  // Opaque — record as itself.
  Operands.push_back(V);
}

/// Collect the SCEVUnknown and SCEVAddRecExpr nodes reachable from a SCEV via
/// LLVM's `SCEVTraversal<>` machinery — no string parsing, no regex.
namespace {
struct SCEVNodeCollector {
  SmallPtrSet<const SCEVUnknown *, 8> Unknowns;
  SmallPtrSet<const SCEVAddRecExpr *, 4> AddRecs;
  bool follow(const SCEV *S) {
    if (auto *U = dyn_cast<SCEVUnknown>(S))
      Unknowns.insert(U);
    if (auto *AR = dyn_cast<SCEVAddRecExpr>(S))
      AddRecs.insert(AR);
    return true; // keep walking
  }
  bool isDone() const { return false; }
};
} // anonymous namespace

/// Structural shape of a trap branch's i1 predicate. Sizes the reach of
/// compiler-side fix opportunities (bounds-check OR/AND forms with a constant
/// or variable bound, and the generic OR-of-AddRec exit recognition).
/// Independent of whether the source BB sits inside a loop.
namespace {
enum class TrapPredicateShape {
  Unknown,                  ///< Could not classify (null cond, etc.).
  SingleICmp,               ///< Single icmp (NumLeafOps <= 2).
  OrBoundsCheckConstBound,  ///< or(uge(X, B), ult(sub(B, X), K_const))
  AndBoundsCheckConstBound, ///< and(ult(X, B), uge(sub(B, X), K_const))
  OrBoundsCheckVarBound,    ///< or(uge(X, B), ult(sub(B, X), K_addrec))
  AndBoundsCheckVarBound,   ///< and(ult(X, B), uge(sub(B, X), K_addrec))
  OrTwoAddRecICmp,          ///< or(icmp(AR_a), icmp(AR_b)) — no sub-arith
  AndTwoAddRecICmp,         ///< and(icmp(AR_a), icmp(AR_b)) — no sub-arith
  OtherMulti,               ///< NumLeafOps >= 4, no recognized structure
};
} // anonymous namespace

static StringRef trapPredicateShapeName(TrapPredicateShape S) {
  switch (S) {
  case TrapPredicateShape::Unknown:
    return "Unknown";
  case TrapPredicateShape::SingleICmp:
    return "SingleICmp";
  case TrapPredicateShape::OrBoundsCheckConstBound:
    return "OrBoundsCheck-ConstBound";
  case TrapPredicateShape::AndBoundsCheckConstBound:
    return "AndBoundsCheck-ConstBound";
  case TrapPredicateShape::OrBoundsCheckVarBound:
    return "OrBoundsCheck-VarBound";
  case TrapPredicateShape::AndBoundsCheckVarBound:
    return "AndBoundsCheck-VarBound";
  case TrapPredicateShape::OrTwoAddRecICmp:
    return "OrTwoAddRecICmp";
  case TrapPredicateShape::AndTwoAddRecICmp:
    return "AndTwoAddRecICmp";
  case TrapPredicateShape::OtherMulti:
    return "OtherMulti";
  }
  llvm_unreachable("unhandled TrapPredicateShape");
}

/// Match the bounded-iterator OR / AND shape:
///   OR  form: or (uge X, B)  (ult (sub B, X), K)
///   AND form: and(ult X, B)  (uge (sub B, X), K)
/// Returns true iff one arm ordering matches the requested (predicate-pair,
/// K-kind) tuple. K-kind is constant for the *ConstK shapes and an L-affine
/// SCEVAddRecExpr for the *VarK shapes.
///
/// icmp operand-swap is not attempted: clang's post-SROA/SimplifyCFG lowering
/// produces canonical (X, B) and (sub, K) orderings. Swapped forms fall through
/// to OtherMulti, the correct conservative classification.
static bool matchBoundsCheckArmsImpl(Value *Arm1, Value *Arm2,
                                     ICmpInst::Predicate ArmAPred,
                                     ICmpInst::Predicate ArmBPred, bool VarK,
                                     ScalarEvolution &SE, Loop *L) {
  using namespace PatternMatch;
  for (int Try = 0; Try < 2; ++Try) {
    Value *A = Try == 0 ? Arm1 : Arm2;
    Value *B = Try == 0 ? Arm2 : Arm1;
    auto *CmpA = dyn_cast<ICmpInst>(A);
    auto *CmpB = dyn_cast<ICmpInst>(B);
    if (!CmpA || !CmpB)
      continue;
    if (CmpA->getPredicate() != ArmAPred)
      continue;
    if (CmpB->getPredicate() != ArmBPred)
      continue;
    Value *X = CmpA->getOperand(0);
    Value *Bound = CmpA->getOperand(1);
    Value *SubV = CmpB->getOperand(0);
    Value *K = CmpB->getOperand(1);
    Value *BoundOfSub = nullptr, *XOfSub = nullptr;
    if (!match(SubV, m_Sub(m_Value(BoundOfSub), m_Value(XOfSub))))
      continue;
    if (Bound != BoundOfSub || X != XOfSub)
      continue;
    if (VarK) {
      if (!L)
        continue;
      if (!SE.isSCEVable(K->getType()))
        continue;
      const SCEV *KS = SE.getSCEV(K);
      if (auto *AR = dyn_cast<SCEVAddRecExpr>(KS))
        if (AR->getLoop() == L && AR->isAffine())
          return true;
      continue;
    }
    if (isa<ConstantInt>(K))
      return true;
  }
  return false;
}

/// True when \p V is an icmp with an operand whose SCEV is an AddRec for \p L.
static bool hasLAddRec(Value *V, ScalarEvolution &SE, Loop *L) {
  auto *Cmp = dyn_cast<ICmpInst>(V);
  if (!Cmp)
    return false;
  for (Value *Op : {Cmp->getOperand(0), Cmp->getOperand(1)}) {
    if (!SE.isSCEVable(Op->getType()))
      continue;
    const SCEV *S = SE.getSCEV(Op);
    if (auto *AR = dyn_cast<SCEVAddRecExpr>(S))
      if (AR->getLoop() == L)
        return true;
  }
  return false;
}

/// Match `or/and(icmp(X), icmp(Y))` where each arm has an operand whose SCEV is
/// an L-AddRec -- the "two unrelated AddRec exits combined by one OR/AND" shape
/// that SCEV's `computeExitLimit` does not fold to `min(BTC_a, BTC_b)`.
/// Returns false when L is null (out-of-loop edges can't have L-AddRec
/// operands).
static bool matchTwoAddRecICmpImpl(Value *Arm1, Value *Arm2,
                                   ScalarEvolution &SE, Loop *L) {
  if (!L)
    return false;
  return hasLAddRec(Arm1, SE, L) && hasLAddRec(Arm2, SE, L);
}

/// Classify a trap branch's i1 predicate by structural shape.
/// Trial order matters: the more-specific bounds-check sub-arithmetic OR/AND
/// shapes (incl. var-K) are attempted before the generic two-AddRec shape, so a
/// sub-arithmetic OR is never reported as `OrTwoAddRecICmp`.
///
/// Uses `m_LogicalOr` / `m_LogicalAnd` to catch both the explicit `or/and i1`
/// form and clang's `select i1` lowering.
///
/// 3+-arm cascades (`or(or(a, b), c)`) report as `OtherMulti`; the
/// simplification fixpoint peels inner forms into recognized shapes on later
/// pipeline iterations.
static TrapPredicateShape
classifyTrapPredicateShape(Value *Cond, ScalarEvolution &SE, Loop *L) {
  using namespace PatternMatch;
  if (!Cond)
    return TrapPredicateShape::Unknown;

  if (isa<ICmpInst>(Cond))
    return TrapPredicateShape::SingleICmp;

  Value *L1 = nullptr, *L2 = nullptr;

  if (match(Cond, m_LogicalOr(m_Value(L1), m_Value(L2)))) {
    if (matchBoundsCheckArmsImpl(L1, L2, ICmpInst::ICMP_UGE, ICmpInst::ICMP_ULT,
                                 /*VarK=*/false, SE, L))
      return TrapPredicateShape::OrBoundsCheckConstBound;
    if (matchBoundsCheckArmsImpl(L1, L2, ICmpInst::ICMP_UGE, ICmpInst::ICMP_ULT,
                                 /*VarK=*/true, SE, L))
      return TrapPredicateShape::OrBoundsCheckVarBound;
    if (matchTwoAddRecICmpImpl(L1, L2, SE, L))
      return TrapPredicateShape::OrTwoAddRecICmp;
    return TrapPredicateShape::OtherMulti;
  }

  if (match(Cond, m_LogicalAnd(m_Value(L1), m_Value(L2)))) {
    if (matchBoundsCheckArmsImpl(L1, L2, ICmpInst::ICMP_ULT, ICmpInst::ICMP_UGE,
                                 /*VarK=*/false, SE, L))
      return TrapPredicateShape::AndBoundsCheckConstBound;
    if (matchBoundsCheckArmsImpl(L1, L2, ICmpInst::ICMP_ULT, ICmpInst::ICMP_UGE,
                                 /*VarK=*/true, SE, L))
      return TrapPredicateShape::AndBoundsCheckVarBound;
    if (matchTwoAddRecICmpImpl(L1, L2, SE, L))
      return TrapPredicateShape::AndTwoAddRecICmp;
    return TrapPredicateShape::OtherMulti;
  }

  return TrapPredicateShape::OtherMulti;
}

/// HEURISTIC that estimates which trap checks are likely necessary validation
/// traps for incoming (function-argument) values, as opposed to in-loop
/// elimination candidates. Returns true iff \p BB sits within
/// `EntryProximityDepth` dominator steps of the function's entry block OR any
/// loop preheader. A trap at/above a preheader is structurally like
/// entry-level parameter validation -- on the pre-loop boundary, never paid for
/// by the loop body.
static bool isEntryProximate(const Function &F, BasicBlock *BB,
                             const DominatorTree &DT, const LoopInfo &LI) {
  if (!BB)
    return false;
  auto *N = DT.getNode(BB);
  if (!N)
    return false;
  unsigned Depth = 0;
  for (auto *Cur = N; Cur; Cur = Cur->getIDom()) {
    BasicBlock *CurBB = Cur->getBlock();
    // Function entry: classical entry-proximity boundary.
    if (CurBB == &F.getEntryBlock())
      return true;
    // Loop preheader: also a validation boundary. It sits outside the loop,
    // and trap edges at/above it run once per outer entry to the nest,
    // regardless of inner-loop iteration counts.
    if (BasicBlock *Succ = CurBB->getSingleSuccessor()) {
      if (Loop *SuccL = LI.getLoopFor(Succ))
        if (SuccL->getHeader() == Succ && SuccL->getLoopPreheader() == CurBB)
          return true;
    }
    if (Depth >= EntryProximityDepth.getValue())
      return false;
    ++Depth;
  }
  return false;
}

/// Decide whether an equivalent check on a dominating path already guarantees
/// this trap cannot fire. Returns true iff some dominator of \p BB ends in a
/// conditional branch whose condition is the same as \p MyCond -- either the
/// identical SSA value, or a structurally equivalent icmp (same predicate and
/// operand pointers) -- so the trap predicate has already been decided before
/// control reaches \p BB.
static bool isDominatedByEquivalentCheck(BasicBlock *BB, Value *MyCond,
                                         const DominatorTree &DT) {
  if (!BB || !MyCond)
    return false;
  auto *MyCmp = dyn_cast<ICmpInst>(MyCond);
  auto *N = DT.getNode(BB);
  if (!N)
    return false;
  // Walk strict dominators (skip BB itself), bounding cost on functions with
  // very deep dominator chains.
  const unsigned MaxDomChains = 16;
  unsigned Steps = 0;
  for (auto *Cur = N->getIDom(); Cur && Steps < MaxDomChains;
       Cur = Cur->getIDom(), ++Steps) {
    BasicBlock *Dom = Cur->getBlock();
    auto *DomBI = dyn_cast<CondBrInst>(Dom->getTerminator());
    if (!DomBI)
      continue;
    Value *DomCond = DomBI->getCondition();
    bool Equivalent = (DomCond == MyCond);
    if (!Equivalent && MyCmp)
      if (auto *DomCmp = dyn_cast<ICmpInst>(DomCond))
        Equivalent = DomCmp->getPredicate() == MyCmp->getPredicate() &&
                     DomCmp->getOperand(0) == MyCmp->getOperand(0) &&
                     DomCmp->getOperand(1) == MyCmp->getOperand(1);
    if (!Equivalent)
      continue;
    if (!LTAEmitExplain)
      return true;
    // Require the dominating branch to actually DETERMINE the condition on the
    // path to BB (one of its edges dominates BB). A same-condition dominator
    // whose neither edge dominates BB proves nothing, so calling the trap
    // "redundant" would be unsound.
    BasicBlockEdge TrueEdge(Dom, DomBI->getSuccessor(0));
    BasicBlockEdge FalseEdge(Dom, DomBI->getSuccessor(1));
    if (DT.dominates(TrueEdge, BB) || DT.dominates(FalseEdge, BB))
      return true;
  }
  return false;
}

/// Walk the def chain feeding \p Start and confirm every in-loop instruction on
/// it lives in a BB that dominates \p Latch. Used to decide whether an IV's
/// update is unconditional (dominates the latch) or control-dependent.
static bool allUpdatesDominate(Value *Start, Loop *L, BasicBlock *Header,
                               BasicBlock *Latch, const DominatorTree &DT) {
  SmallPtrSet<const Value *, 32> Seen;
  SmallVector<const Value *, 16> Stack;
  Stack.push_back(Start);
  unsigned Steps = 0;
  const unsigned StepLimit = 64;
  while (!Stack.empty() && Steps++ < StepLimit) {
    const Value *V = Stack.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    const auto *I = dyn_cast<Instruction>(V);
    if (!I)
      continue;
    if (!L->contains(I->getParent()))
      continue;
    // The header phi's parent IS the header, which dominates the latch;
    // don't recurse through it (recurrence already inspected at the call
    // site).
    if (isa<PHINode>(I) && I->getParent() == Header)
      continue;
    // Any other chain instruction must live in a BB dominating the latch;
    // otherwise the update is control-dependent.
    if (!DT.dominates(I->getParent(), Latch))
      return false;
    // Walk operands. For non-header (merge) phis this recurses into both
    // arms — catching the `m += cond ? 1 : 0` shape where one arm's add sits
    // in a non-dominating BB.
    for (const Use &U : I->operands())
      Stack.push_back(U.get());
  }
  return true;
}

/// For each IV recurrence feeding trap-branch condition \p Cond, decide whether
/// its update (the latch-incoming value) dominates the loop latch. Returns
/// false as soon as one conditionally-updated IV is found; true (the default)
/// when there is no loop, no latch, or every reachable update dominates the
/// latch.
static bool computeIVUpdateDominatesLatch(Value *Cond, Loop *L,
                                          const DominatorTree &DT) {
  if (!L)
    return true;
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return true;
  SmallPtrSet<const Value *, 32> Seen;
  SmallVector<const Value *, 16> Stack;
  Stack.push_back(Cond);
  unsigned Steps = 0;
  const unsigned StepLimit = 64;
  while (!Stack.empty() && Steps++ < StepLimit) {
    const Value *V = Stack.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    const auto *I = dyn_cast<Instruction>(V);
    if (!I)
      continue;
    if (!L->contains(I->getParent()))
      continue;
    if (const auto *Phi = dyn_cast<PHINode>(I)) {
      if (Phi->getParent() == Header) {
        if (Value *In = Phi->getIncomingValueForBlock(Latch))
          if (!allUpdatesDominate(In, L, Header, Latch, DT))
            return false;
        // Don't recurse through phi operands — recurrence just inspected via
        // the latch incoming.
        continue;
      }
    }
    for (const Use &U : I->operands())
      Stack.push_back(U.get());
  }
  return true;
}

/// Emit one LoopTrapEdge remark per conditional branch whose one successor is a
/// trap block (see isTrapEdgeBlock). Gated by -loop-trap-analysis-explain. The
/// record's fields are the per-edge axes described at the top of the file.
static void emitPerTrapEdge(Function &F, LoopInfo &LI,
                            OptimizationRemarkEmitter &ORE, ScalarEvolution &SE,
                            const DominatorTree &DT) {
  std::string Name = "LoopTrapEdge";
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<CondBrInst>(BB.getTerminator());
    if (!BI)
      continue;
    BasicBlock *TrapSucc = nullptr;
    for (BasicBlock *Succ : BI->successors())
      if (isTrapEdgeBlock(Succ)) {
        TrapSucc = Succ;
        break;
      }
    if (!TrapSucc)
      continue;

    Loop *Innermost = LI.getLoopFor(&BB);
    // Chain of containing loops for the source BB, innermost first. Used to
    // decide whether an operand's SCEVUnknown is defined inside the nest.
    SmallVector<Loop *, 4> ContainingLoops;
    for (Loop *L = Innermost; L; L = L->getParentLoop())
      ContainingLoops.push_back(L);

    bool IsLoopExit = Innermost && !Innermost->contains(TrapSucc);

    // Predicate-tree shape (in-loop and out-of-loop edges alike). Sizes the
    // reach of shape-specific levers: NUW propagation on a bounds-check OR
    // targets OrBoundsCheckConstBound; the AND-form lever targets
    // AndBoundsCheckConstBound; the variable-bound extension targets the
    // *BoundsCheckVarBound shapes; a generic SCEV exit-count extension targets
    // {Or,And}TwoAddRecICmp.
    TrapPredicateShape PredShape =
        classifyTrapPredicateShape(BI->getCondition(), SE, Innermost);

    SmallVector<Value *, 8> LeafOperands;
    SmallPtrSet<Value *, 16> Visited;
    collectBoolLeafOperands(BI->getCondition(), LeafOperands, Visited);
    unsigned NumLeafOps = LeafOperands.size();

    // SCEV-descriptive fields for the predicate's leaf operands:
    //   SCEVComputed        — every non-constant leaf had a computable SCEV.
    //   SCEVLoopInvariant   — every leaf SCEV is invariant in the innermost
    //                         containing loop (trivially true if no loop).
    //   HasAddRec           — some leaf SCEV contains an AddRec.
    //   HasInLoopUnknown    — some leaf SCEV has a SCEVUnknown defined in a
    //                         containing loop.
    // Stride-fragility flags for any affine L-AddRec leaf operand break down
    // the weak-no-wrap class ("operands clean for L but getExitCount still
    // CouldNotCompute"):
    //   HasNonUnitStrideForLAddRec  — an L-AddRec operand with |step| > 1.
    //   HasNegativeStrideForLAddRec — an L-AddRec with a negative const step.
    //   HasNonConstantStrideForLAddRec — an L-AddRec with a runtime step.
    //   HasOnlyNotProvenMonotonicForLAddRec — an L-AddRec with FlagNW but
    //                                 neither NUW nor NSW.
    bool AllComputed = true;
    bool LoopInvariantInInnermost = true; // trivially true if no loop
    bool HasAddRec = false;
    bool HasInLoopUnknown = false;
    bool HasNonUnitStrideForLAddRec = false;
    bool HasNegativeStrideForLAddRec = false;
    bool HasNonConstantStrideForLAddRec = false;
    bool HasOnlyNotProvenMonotonicForLAddRec = false;
    for (Value *V : LeafOperands) {
      if (!V || isa<Constant>(V))
        continue;
      if (!SE.isSCEVable(V->getType())) {
        AllComputed = false;
        continue;
      }
      const SCEV *SC = SE.getSCEV(V);
      if (isa<SCEVCouldNotCompute>(SC)) {
        AllComputed = false;
        continue;
      }
      SCEVNodeCollector Coll;
      SCEVTraversal<SCEVNodeCollector>(Coll).visitAll(SC);
      if (!Coll.AddRecs.empty())
        HasAddRec = true;
      // Per-loop invariance: SE.isLoopInvariant against the innermost
      // containing loop only (single canonical bool).
      if (Innermost && !SE.isLoopInvariant(SC, Innermost))
        LoopInvariantInInnermost = false;
      // Any SCEVUnknown whose defining instruction lives in a containing loop
      // (invariant in the innermost, but a load in an outer loop still changes
      // per-outer-iteration).
      for (const SCEVUnknown *U : Coll.Unknowns) {
        auto *I = dyn_cast_or_null<Instruction>(U->getValue());
        if (!I)
          continue;
        Loop *DefLoop = LI.getLoopFor(I->getParent());
        for (Loop *DL = DefLoop; DL && !HasInLoopUnknown;
             DL = DL->getParentLoop())
          for (Loop *CL : ContainingLoops)
            if (DL == CL) {
              HasInLoopUnknown = true;
              break;
            }
        if (HasInLoopUnknown)
          break;
      }
      if (Innermost) {
        for (const SCEVAddRecExpr *AR : Coll.AddRecs) {
          if (AR->getLoop() != Innermost || !AR->isAffine())
            continue;
          // FlagNW only (no NUW / NSW) — typical for AddRecs from
          // `add invariant, mul(IV, const)` where the outer add lacks IR-level
          // nowrap.
          if (!AR->hasNoUnsignedWrap() && !AR->hasNoSignedWrap())
            HasOnlyNotProvenMonotonicForLAddRec = true;
          if (auto *StepC = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE))) {
            const APInt &Step = StepC->getAPInt();
            if (Step.isNegative())
              HasNegativeStrideForLAddRec = true;
            // |step| > 1 — non-unit stride, fragile for getExitCount.
            APInt AbsStep = Step.isNegative() ? -Step : Step;
            if (AbsStep.ugt(1))
              HasNonUnitStrideForLAddRec = true;
          } else {
            // Non-constant (runtime) step: closed-form trip count essentially
            // never computable. Tag both the umbrella non-unit flag and the
            // specific non-constant-stride flag.
            HasNonUnitStrideForLAddRec = true;
            HasNonConstantStrideForLAddRec = true;
          }
        }
      }
    }

    // Per-edge exit count. EdgeBTCComputable reflects the per-edge SCEV exit
    // count for THIS exiting block (SE.getExitCount for &BB), not the loop's
    // overall latch/backedge-taken count. EdgeBTCSymbolic is the
    // SymbolicMaximum bucket, checked only when the exact count is not known.
    bool EdgeBTCComputable = false;
    bool EdgeBTCSymbolic = false;
    if (Innermost && IsLoopExit) {
      const SCEV *EC = SE.getExitCount(Innermost, &BB);
      EdgeBTCComputable = !isa<SCEVCouldNotCompute>(EC);
      if (!EdgeBTCComputable) {
        const SCEV *SymEC =
            SE.getExitCount(Innermost, &BB, ScalarEvolution::SymbolicMaximum);
        EdgeBTCSymbolic = !isa<SCEVCouldNotCompute>(SymEC);
      }
    }

    // Out-of-loop / structural-redundancy fields. Also computed for in-loop
    // edges (cheap, and an in-loop trap dominated by an equivalent earlier
    // check is still a CVP / dominator-fold candidate).
    bool IsEntryProx = isEntryProximate(F, &BB, DT, LI);
    bool DomByEquiv = isDominatedByEquivalentCheck(&BB, BI->getCondition(), DT);

    // Q1 (dominance): does the trap branch's BB dominate the latch? If yes the
    // cond fires every iteration; if no it's in a conditional arm and LICM
    // can't hoist here. Q2 (IV update): for any IV operand, does its update
    // dominate the latch? If no, the IV is conditionally updated and SCEV
    // refuses an AddRec → trap not eliminable via trip-count.
    // LoopLatchBTCComputable is the latch exit count SE.getExitCount(L, Latch):
    // the IV-driven count, which can be known when the loop-wide backedge-taken
    // count is not (one unknown early trap exit blocks the latter, not this).
    bool DominatesLatch = false;
    bool IVUpdateDominatesLatch = true;
    bool LoopLatchBTCComputable = false;
    if (LTAEmitExplain) {
      BasicBlock *Latch = Innermost ? Innermost->getLoopLatch() : nullptr;
      DominatesLatch = Latch && DT.dominates(&BB, Latch);
      IVUpdateDominatesLatch =
          computeIVUpdateDominatesLatch(BI->getCondition(), Innermost, DT);
      if (Latch)
        LoopLatchBTCComputable =
            !isa<SCEVCouldNotCompute>(SE.getExitCount(Innermost, Latch));
    }

    OptimizationRemarkAnalysis Rem(REMARK_PASS, Name, BI);
    Rem << "Function " << NV("Function", F.getName())
        << " src_bb=" << NV("SourceBB", bbLabel(&BB))
        << " trap_bb=" << NV("TrapBB", bbLabel(TrapSucc)) << " loop_depth="
        << NV("LoopDepth", Innermost ? Innermost->getLoopDepth() : 0u)
        << " loop_header="
        << NV("LoopHeader",
              Innermost ? bbLabel(Innermost->getHeader()) : std::string(""))
        << " is_innermost="
        << NV("IsInnermost", (bool)(Innermost && Innermost->isInnermost()))
        << " is_loop_exit=" << NV("IsLoopExit", IsLoopExit)
        << " num_leaf_operands=" << NV("NumLeafOperands", NumLeafOps)
        << " scev_computed=" << NV("SCEVComputed", AllComputed)
        << " scev_loop_invariant="
        << NV("SCEVLoopInvariant", LoopInvariantInInnermost)
        << " has_addrec=" << NV("HasAddRec", HasAddRec)
        << " has_in_loop_unknown=" << NV("HasInLoopUnknown", HasInLoopUnknown)
        << " has_non_unit_stride_for_l_addrec="
        << NV("HasNonUnitStrideForLAddRec", HasNonUnitStrideForLAddRec)
        << " has_non_constant_stride_for_l_addrec="
        << NV("HasNonConstantStrideForLAddRec", HasNonConstantStrideForLAddRec)
        << " has_only_weak_no_wrap_for_l_addrec="
        << NV("HasOnlyNotProvenMonotonicForLAddRec",
              HasOnlyNotProvenMonotonicForLAddRec)
        << " has_negative_stride_for_l_addrec="
        << NV("HasNegativeStrideForLAddRec", HasNegativeStrideForLAddRec)
        << " edge_btc_computable=" << NV("EdgeBTCComputable", EdgeBTCComputable)
        << " edge_btc_symbolic=" << NV("EdgeBTCSymbolic", EdgeBTCSymbolic)
        << " predicate_shape="
        << NV("PredicateShape", trapPredicateShapeName(PredShape).str())
        << " is_entry_proximate=" << NV("IsEntryProximate", IsEntryProx)
        << " dominated_by_equivalent_check="
        << NV("DominatedByEquivalentCheck", DomByEquiv);
    if (LTAEmitExplain) {
      Rem << " dominates_latch=" << NV("DominatesLatch", DominatesLatch)
          << " iv_update_dominates_latch="
          << NV("IVUpdateDominatesLatch", IVUpdateDominatesLatch)
          << " loop_latch_btc_computable="
          << NV("LoopLatchBTCComputable", LoopLatchBTCComputable);
    }
    ORE.emit(Rem);
  }
}

/// Check for an unreachable instruction that has an edge to any of \p L basic
/// blocks. if `--use-bounds-safety-traps-only` is used make sure that the trap and
/// branch instructions have -fbounds-safety annotation.
static bool hasUnreachableInst(Loop *L) {
  SmallVector<BasicBlock *, 4> LoopExitBlocks;
  L->getExitBlocks(LoopExitBlocks);
  for (auto *BB : LoopExitBlocks) {
    auto *I = BB->getTerminator();
    // check for trap instructions. If `BoundsSafetyTrapsOnly` is false then we
    // ignore if the trap has a -fbounds-safety annotation.
    if (!isa<UnreachableInst>(I))
      continue;
    if (BoundsSafetyTrapsOnly && !isBoundsSafetyAnnotated(I))
      continue;
    if (any_of(predecessors(BB), [L](BasicBlock *PredB) {
          auto *TerminatorInst = PredB->getTerminator();
          return L->contains(PredB) &&
                 (isa<CondBrInst>(TerminatorInst) ||
                  isa<UncondBrInst>(TerminatorInst) ||
                  isa<SwitchInst>(TerminatorInst)) &&
                 (!BoundsSafetyTrapsOnly ||
                  isBoundsSafetyAnnotated(TerminatorInst));
        }))
      return true;
  }
  return false;
}

static std::string getSideEffectReasons(const Instruction &I) {
  std::string Buf;
  raw_string_ostream OS(Buf);
  if (isa<CallInst>(I) && I.mayReadOrWriteMemory())
    OS << "Instruction might have a volatile memory access";
  else {
    if (I.mayWriteToMemory())
      OS << "Instruction may write to memory\n";
    if (I.mayThrow())
      OS << "Instruction may throw an exception\n";
    if (!I.willReturn())
      OS << "Instruction may not return\n";
  }
  return Buf;
}

/// Check if \p L can be hoisted or not and emit a detailed remark about why
/// it can't be hoisted.
static CheckLoopHoistType processLoops(Loop *L, ScalarEvolution &SE,
                                       OptimizationRemarkEmitter &ORE) {
  CheckLoopHoistType HoistType;
  bool SymbolicMaxBackEdgeComputable =
      !isa<SCEVCouldNotCompute>(SE.getSymbolicMaxBackedgeTakenCount(L));
  bool HasSideEffects = false;
  if (!hasUnreachableInst(L))
    return CheckLoopHoistType::SKIP;

  SmallVector<std::string, 4> InstructionsWithSideEffects;
  SmallVector<std::string, 4> SideEffectReasons;
  for (auto *BB : L->blocks()) {
    if (any_of(*BB, [&InstructionsWithSideEffects,
                     &SideEffectReasons](const Instruction &I) {
          bool InstHasSideEffects = false;
          // If a call instruction reads or writes to memory we don't know if
          // the access is non-volatile so we asssume that the call instruction
          // has side effects.
          if (isa<CallInst>(I))
            InstHasSideEffects =
                I.mayHaveSideEffects() || I.mayReadFromMemory();
          else if (NewTrapSemantics)
            InstHasSideEffects = !I.willReturn() || I.mayThrow();
          else
            InstHasSideEffects = I.mayHaveSideEffects();
          if (InstHasSideEffects) {
            std::string Buf;
            raw_string_ostream OS(Buf);
            I.print(OS);
            InstructionsWithSideEffects.push_back(Buf);
            SideEffectReasons.push_back(getSideEffectReasons(I));
          }
          return InstHasSideEffects;
        })) {

      HasSideEffects = true;
      break;
    }
  }

  HoistType = !HasSideEffects && SymbolicMaxBackEdgeComputable
                  ? CheckLoopHoistType::MAYBE_CAN_HOIST
                  : CheckLoopHoistType::CANNOT_HOIST;
  // Emit a remark for the loop
  auto ORA = OptimizationRemarkAnalysis(REMARK_PASS, "LoopTrap",
                                        &L->getHeader()->front());
  ORA << "Loop: " << L->getName() << " ";
  if (HoistType == CheckLoopHoistType::CANNOT_HOIST) {
    ORA << "cannot be hoisted: \n";
    if (HasSideEffects) {
      ORA << "\nThe following instructions have side effects:\n";
      for (unsigned Idx = 0; Idx < InstructionsWithSideEffects.size(); Idx++) {
        ORA << "\t" << InstructionsWithSideEffects[Idx] << "\n";
        ORA << "Reason:\n";
        ORA << SideEffectReasons[Idx];
      }
    }
    if (!SymbolicMaxBackEdgeComputable)
      ORA << "Backedge is not computable.\n";
  } else
    ORA << "can be hoisted\n";
  ORE.emit(ORA);
  return HoistType;
}

/// Collect info for hoistable loop checks for \p F and report remarks for
/// individual loops and report a summary for hoistable checks for the function.
static void emitRemarks(Function &F, LoopInfo &LI,
                        OptimizationRemarkEmitter &ORE, ScalarEvolution &SE) {
  unsigned TotalCanHoistLoops = 0;
  unsigned TotalUnHoistableLoops = 0;
  for (auto *L : LI.getLoopsInPreorder()) {
    CheckLoopHoistType Type = processLoops(L, SE, ORE);
    if (Type == CheckLoopHoistType::MAYBE_CAN_HOIST)
      TotalCanHoistLoops++;
    else if (Type == CheckLoopHoistType::CANNOT_HOIST)
      TotalUnHoistableLoops++;
  }

  OptimizationRemarkAnalysis Rem(REMARK_PASS, "LoopTrapSummary", &F);
  Rem << "Trap checks results:\n";
  Rem << "Total count of loops with traps "
      << NV("TotalCount", TotalCanHoistLoops + TotalUnHoistableLoops) << "\n";
  Rem << "Loops that maybe can be hoisted: "
      << NV("CountHoist", TotalCanHoistLoops) << "\n";
  Rem << "Loops that cannot be hoisted: "
      << NV("CountCannotHoist", TotalUnHoistableLoops) << "\n";
  ORE.emit(Rem);
}

PreservedAnalyses LoopTrapAnalysisPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  emitRemarks(F, LI, ORE, SE);
  if (LTAEmitExplain) {
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    emitLoopPrimitives(F, LI, ORE, SE);
    emitPerTrapEdge(F, LI, ORE, SE, DT);
  }
  return PreservedAnalyses::all();
}

void LoopTrapAnalysisPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  OS << "loop-trap-analysis";
}
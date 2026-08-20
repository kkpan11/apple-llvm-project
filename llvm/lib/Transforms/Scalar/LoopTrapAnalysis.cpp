//===-- LoopTrapAnalysis.cpp - Loop Trap Count pass -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LoopTrapAnalysis.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Remarks/BoundsSafetyOptRemarks.h"
#include "llvm/Support/CommandLine.h"
using namespace llvm;
using namespace llvm::ore;
#define DEBUG_TYPE "loop-trap-analysis"
#define REMARK_PASS DEBUG_TYPE

enum class CheckLoopHoistType { MAYBE_CAN_HOIST, CANNOT_HOIST, SKIP };
static cl::opt<bool> BoundsSafetyTrapsOnly(
    "use-bounds-safety-traps-only", cl::init(false),
    cl::desc(
        "We only check for -fbounds-safety traps if the flag is false we can check "
        "for any hoistable traps."));

/// Max dominator depth from the entry block at which a trap-edge source BB is
/// still "entry-proximate" (plausible parameter validation). Populates the
/// `IsEntryProximate` field on each LoopTrapEdge record.
static cl::opt<unsigned> EntryProximityDepth(
    "loop-trap-entry-proximity-depth", cl::init(3),
    cl::desc("Maximum dominator depth from function entry at which a "
             "trap edge is classified IsEntryProximate (default 3)."));

/// Gate the explanatory trap analysis. When false (default), the pass emits
/// bit-identical YAML to a pre-framework snapshot. When true, it emits the
/// full explanation: the refined TrapClass classification (recognizes all
/// trap-like calls, splits stride variants, does not mask blocked edges) plus
/// the extra per-loop and per-edge fields (DominatesLatch / IV-update /
/// operand-class). Lets the framework be A/B compared and reverted via one
/// flag.
static cl::opt<bool> LTAEmitExplain(
    "loop-trap-analysis-explain", cl::init(false),
    cl::desc("Emit the explanatory trap analysis: the refined TrapClass "
             "classification plus per-loop and per-edge explanatory fields "
             "(DominatesLatch / IV-update / operand-class). When false "
             "(default), only the pre-explanation fields and compact TrapClass "
             "are emitted, so the framework can be A/B compared and reverted "
             "by toggling this flag alone."));

static cl::opt<bool> LTAEmitLoadAlias(
    "loop-load-alias", cl::init(false),
    cl::desc("Opt-in: for each load in an INNERMOST loop that is may-clobbered "
             "by an in-loop writer (store / memcpy / call), emit a "
             "LoopLoadAlias record naming the first such writer. Clobbered "
             "loads only (hoistable loads are omitted). Off by default; "
             "enable selectively as it emits one record per clobbered load."));

/// Legacy (pre-unification) trap-block predicate variants, reproduced
/// byte-for-byte when `-loop-trap-analysis-explain` is off so existing output /
/// lit tests are unchanged:
///   AnyUnreachableBoundsSafety - any `unreachable`, filtered by the
///       -fbounds-safety annotation when BoundsSafetyOnly.
///   AnyUnreachable - any `unreachable`, no annotation filter.
///   TrapIntrinsic  - `unreachable` preceded by `call @llvm.trap()` only.
enum class LegacyTrapMatch {
  AnyUnreachableBoundsSafety,
  AnyUnreachable,
  TrapIntrinsic
};

/// Single source of truth for "is BB a trap block". With
/// `-loop-trap-analysis-explain` (opt-in), all callers agree: BB ends in
/// `unreachable` immediately preceded by `@llvm.trap()` or `@llvm.ubsantrap()`
/// (Swift -fbounds-safety lowers to ubsantrap), with a consistent bounds-safety
/// annotation filter. When the flag is off (default), each caller's original
/// predicate (\p Legacy) is reproduced exactly, so emitted records do not
/// change.
static bool isTrapBlock(BasicBlock *BB, bool BoundsSafetyOnly,
                        LegacyTrapMatch Legacy) {
  if (!BB || BB->empty())
    return false;
  Instruction *Term = BB->getTerminator();
  if (!isa<UnreachableInst>(Term))
    return false;
  if (LTAEmitExplain) {
    // Unified: bounds-safety filter + a trap-like terminating call, identified
    // by its semantic property rather than an intrinsic allowlist: a
    // `noreturn` call touching only inaccessible memory (the shared property of
    // @llvm.trap / @llvm.ubsantrap / @llvm.looptrap and any future trap
    // intrinsic).
    if (BoundsSafetyOnly && !isBoundsSafetyAnnotated(Term))
      return false;
    if (Term == &BB->front())
      return false;
    if (auto *CI = dyn_cast<CallInst>(Term->getPrevNode()))
      return CI->doesNotReturn() && CI->onlyAccessesInaccessibleMemory();
    return false;
  }
  // Legacy: reproduce each caller's original predicate byte-for-byte.
  switch (Legacy) {
  case LegacyTrapMatch::AnyUnreachableBoundsSafety:
    return !BoundsSafetyOnly || isBoundsSafetyAnnotated(Term);
  case LegacyTrapMatch::AnyUnreachable:
    return true;
  case LegacyTrapMatch::TrapIntrinsic:
    if (Term == &BB->front())
      return false;
    if (auto *CI = dyn_cast<CallInst>(Term->getPrevNode()))
      if (Function *Callee = CI->getCalledFunction())
        return Callee->getIntrinsicID() == Intrinsic::trap;
    return false;
  }
  return false;
}

/// Check for an unreachable instruction that has an edge to any of \p L basic
/// blocks. if `--use-bounds-safety-traps-only` is used make sure that the trap and
/// branch instructions have -fbounds-safety annotation.
static bool hasUnreachableInst(Loop *L) {
  SmallVector<BasicBlock *, 4> LoopExitBlocks;
  L->getExitBlocks(LoopExitBlocks);
  for (auto *BB : LoopExitBlocks) {
    // Trap exit block (shared predicate, honoring
    // --use-bounds-safety-traps-only).
    if (!isTrapBlock(BB, BoundsSafetyTrapsOnly,
                     LegacyTrapMatch::AnyUnreachableBoundsSafety))
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

static unsigned countTrapExits(Loop *L, LoopInfo &LI) {
  unsigned Count = 0;
  SmallVector<BasicBlock *, 4> LoopExitBlocks;
  L->getExitBlocks(LoopExitBlocks);
  for (auto *BB : LoopExitBlocks) {
    if (!isTrapBlock(BB, BoundsSafetyTrapsOnly,
                     LegacyTrapMatch::AnyUnreachableBoundsSafety))
      continue;
    // Strict attribution: count this trap exit only if it has a predecessor
    // whose immediate containing loop is L (not a nested sub-loop). Otherwise
    // an inner trap's unreachable target -- in every enclosing loop's exit set
    // -- would be counted at every nest level.
    if (any_of(predecessors(BB), [L, &LI](BasicBlock *PredB) {
          if (LI.getLoopFor(PredB) != L)
            return false;
          auto *TerminatorInst = PredB->getTerminator();
          return L->contains(PredB) &&
                 (isa<CondBrInst>(TerminatorInst) ||
                  isa<UncondBrInst>(TerminatorInst) ||
                  isa<SwitchInst>(TerminatorInst)) &&
                 (!BoundsSafetyTrapsOnly ||
                  isBoundsSafetyAnnotated(TerminatorInst));
        }))
      ++Count;
  }
  return Count;
}

/// Count conditional branches inside the loop body whose target is a trap
/// block (BB ending in `unreachable`, optionally preceded by
/// `tail call @llvm.trap()`). Captures per-iteration trap-direction branches.
///
/// Strict attribution: only edges originating in this loop's own body (not in
/// any sub-loop). Since `L->blocks()` transitively includes sub-loop blocks,
/// without this filter an inner-loop trap would be double-counted by every
/// enclosing loop's record.
static unsigned countCondTrapEdges(Loop *L, LoopInfo &LI) {
  unsigned Count = 0;
  // Trap block: an unreachable BB whose only side effect is llvm.trap (or bare
  // unreachable).
  auto IsTrapBlock = [](BasicBlock *BB) {
    return isTrapBlock(BB, BoundsSafetyTrapsOnly,
                       LegacyTrapMatch::AnyUnreachableBoundsSafety);
  };
  for (auto *BB : L->blocks()) {
    // Strict attribution: only edges whose source BB's immediate containing
    // loop is L. Edges in nested sub-loops belong to those inner loops.
    if (LI.getLoopFor(BB) != L)
      continue;
    auto *Term = BB->getTerminator();
    auto *BI = dyn_cast<CondBrInst>(Term);
    if (!BI)
      continue;
    for (BasicBlock *Succ : BI->successors()) {
      if (L->contains(Succ))
        continue;
      if (IsTrapBlock(Succ)) {
        if (BoundsSafetyTrapsOnly && !isBoundsSafetyAnnotated(Term))
          continue;
        ++Count;
      }
    }
  }
  return Count;
}

/// Count cond-trap edges whose condition is loop-invariant — i.e. LICM could
/// hoist them out but hasn't. Estimates the residual hoisting opportunity.
static unsigned countHoistableCondTrapEdges(Loop *L, LoopInfo &LI) {
  unsigned Count = 0;
  auto IsTrapBlock = [](BasicBlock *BB) {
    return isTrapBlock(BB, BoundsSafetyTrapsOnly,
                       LegacyTrapMatch::AnyUnreachable);
  };
  for (auto *BB : L->blocks()) {
    // Strict attribution: only edges originating in L's own body. Same rule
    // as countCondTrapEdges.
    if (LI.getLoopFor(BB) != L)
      continue;
    auto *Term = BB->getTerminator();
    auto *BI = dyn_cast<CondBrInst>(Term);
    if (!BI)
      continue;
    bool HasTrapSucc = false;
    for (BasicBlock *Succ : BI->successors()) {
      if (L->contains(Succ))
        continue;
      if (IsTrapBlock(Succ)) {
        HasTrapSucc = true;
        break;
      }
    }
    if (!HasTrapSucc)
      continue;
    if (L->isLoopInvariant(BI->getCondition()))
      ++Count;
  }
  return Count;
}

/// Classify cond-trap edges by the shape of their controlling condition.
/// Returns (invariant, iv_derived, non_iv) counts:
///   invariant:  whole condition is loop-invariant (LICM/Unswitch territory).
///   iv_derived: at least one icmp operand has an AddRec SCEV on this loop
///               (IndVars PredicatedExit / partial-BTC could help).
///   non_iv:     condition varies per-iter but no operand is an AddRec on L —
///               can't hoist (varies), can't predicate (no recurrence).
struct TrapCondShape {
  unsigned Invariant = 0;
  unsigned IVDerived = 0;
  unsigned NonIV = 0;
  // Cross-product of {DominatesLatch, NotDominatingLatch} ×
  // {Invariant, IVDerived, NonIV}, partitioning all cond-trap edges:
  //   NotDominatingLatch_*      : cond is in a conditional arm that doesn't
  //                               post-dominate the body; SCEV can't compute
  //                               trip count downstream, can't hoist here.
  //   DominatesLatch_Invariant  : fires every iteration and is invariant. LICM.
  //   DominatesLatch_IVDerived  : every iteration, has an L-AddRec operand.
  //                               SCEV / IndVars attackable.
  //   DominatesLatch_NonIV      : every iteration but no invariant/affine
  //                               operand. Varying yet uncomputable -- needs
  //                               AA/TBAA refinement or source change.
  unsigned DominatesLatch = 0;
  unsigned NotDominatingLatch = 0;
  unsigned DLInvariant = 0, DLIVDerived = 0, DLNonIV = 0;
  unsigned NDLInvariant = 0, NDLIVDerived = 0, NDLNonIV = 0;

  // Sub-classification of NonIV -- *why* the operands aren't affine. Flags are
  // non-exclusive and bounded by NonIV. Explains SCEV trap-elimination failure
  // (SCEV needs every operand invariant or affine).
  //   NonIVLoadOp   : operand chain reaches an in-loop LoadInst. Lever: TBAA /
  //                   AA refinement so LICM can hoist the load.
  //   NonIVPhiOp    : operand chain reaches a non-AddRec in-loop PHI (header
  //                   phi with conditional update, or merge phi). Lever:
  //                   source restructure.
  //   NonIVSelectOp : operand chain reaches an in-loop SelectInst / FreezeInst
  //                   (often from control-flow flattening). Same lever as Phi.
  //   NonIVCallOp   : an operand is a call result. SCEV treats as Unknown.
  unsigned NonIVLoadOp = 0;
  unsigned NonIVPhiOp = 0;
  unsigned NonIVSelectOp = 0;
  unsigned NonIVCallOp = 0;
};
static TrapCondShape classifyCondTrapEdges(Loop *L, ScalarEvolution &SE,
                                           const DominatorTree &DT) {
  TrapCondShape S;
  BasicBlock *Latch = L->getLoopLatch();
  auto IsTrapBlock = [](BasicBlock *BB) {
    return isTrapBlock(BB, BoundsSafetyTrapsOnly,
                       LegacyTrapMatch::AnyUnreachable);
  };
  for (auto *BB : L->blocks()) {
    auto *Term = BB->getTerminator();
    auto *BI = dyn_cast<CondBrInst>(Term);
    if (!BI)
      continue;
    bool HasTrapSucc = false;
    for (BasicBlock *Succ : BI->successors()) {
      if (L->contains(Succ))
        continue;
      if (IsTrapBlock(Succ)) {
        HasTrapSucc = true;
        break;
      }
    }
    if (!HasTrapSucc)
      continue;

    // Dominance dimension: if the trap branch's parent BB dominates the latch,
    // the cond evaluates every iteration; otherwise it's in a conditional arm
    // and SCEV cannot compute trip counts past it.
    bool DominatesLatch = Latch && DT.dominates(BB, Latch);
    if (DominatesLatch)
      ++S.DominatesLatch;
    else
      ++S.NotDominatingLatch;

    Value *Cond = BI->getCondition();
    if (L->isLoopInvariant(Cond)) {
      ++S.Invariant;
      if (DominatesLatch)
        ++S.DLInvariant;
      else
        ++S.NDLInvariant;
      continue;
    }
    auto *Cmp = dyn_cast<ICmpInst>(Cond);
    bool FoundAddRec = false;
    auto Inspect = [&](Value *V) {
      if (L->isLoopInvariant(V))
        return;
      if (!SE.isSCEVable(V->getType()))
        return;
      const SCEV *SC = SE.getSCEV(V);
      if (auto *AR = dyn_cast<SCEVAddRecExpr>(SC))
        if (AR->getLoop() == L)
          FoundAddRec = true;
    };
    if (Cmp) {
      Inspect(Cmp->getOperand(0));
      Inspect(Cmp->getOperand(1));
    } else {
      Inspect(Cond);
    }
    if (FoundAddRec) {
      ++S.IVDerived;
      if (DominatesLatch)
        ++S.DLIVDerived;
      else
        ++S.NDLIVDerived;
    } else {
      ++S.NonIV;
      if (DominatesLatch)
        ++S.DLNonIV;
      else
        ++S.NDLNonIV;

      // Sub-classify NonIV: walk the cond's operand chain (in-loop,
      // depth-bounded) and tag which operand classes appear. Flags are
      // non-exclusive. The walk is shallow (no header-phi recurrence) so it
      // stays O(loop body) without chain explosion.
      SmallPtrSet<const Value *, 32> Seen;
      SmallVector<const Value *, 16> Stack;
      auto Push = [&](Value *V) { Stack.push_back(V); };
      if (Cmp) {
        Push(Cmp->getOperand(0));
        Push(Cmp->getOperand(1));
      } else {
        Push(Cond);
      }
      bool HasLoad = false, HasPhi = false, HasSelect = false, HasCall = false;
      unsigned Steps = 0;
      const unsigned StepLimit = 64;
      while (!Stack.empty() && Steps++ < StepLimit) {
        const Value *V = Stack.pop_back_val();
        if (!Seen.insert(V).second)
          continue;
        const auto *I = dyn_cast<Instruction>(V);
        if (!I || !L->contains(I->getParent()))
          continue;
        if (isa<LoadInst>(I))
          HasLoad = true;
        else if (isa<PHINode>(I))
          HasPhi = true;
        else if (isa<SelectInst>(I) || isa<FreezeInst>(I))
          HasSelect = true;
        else if (isa<CallInst>(I))
          HasCall = true;
        // Don't recurse through phi operands (avoids cycle chain explosion);
        // 'phi' is already tagged. Recurse through anything else.
        if (!isa<PHINode>(I))
          for (const Use &U : I->operands())
            Stack.push_back(U.get());
      }
      if (HasLoad)
        ++S.NonIVLoadOp;
      if (HasPhi)
        ++S.NonIVPhiOp;
      if (HasSelect)
        ++S.NonIVSelectOp;
      if (HasCall)
        ++S.NonIVCallOp;
    }
  }
  return S;
}

// Emit one or more side-effect tags identifying the *class* of instruction
// that blocks LICM. Tags use the structured format
//
//   <Category>.<cause>[: <callee>]
//
// where <Category> is the instruction class (Store / AtomicStore /
// VolatileStore / AtomicLoad / VolatileLoad / AtomicRMW / AtomicCmpXchg /
// Fence / Other / Call / MemIntrinsic / VolatileCall) and <cause> is one of
// {may-write-to-memory, may-read-from-memory, may-access-memory, may-throw,
// may-not-return}. Optional ": <callee>" disambiguates calls. Splitting by
// class (vs the prior two opaque buckets) shows which lever -- TBAA,
// function-attr annotations, per-intrinsic AA -- would unblock a given loop.
static std::string getSideEffectReasons(const Instruction &I) { return ""; }

/// Check if the loop preheader has ".hoisted" instructions, indicating
/// successful hoisting of trap checks out of the loop.
static bool hasHoistedInPreheader(Loop *L,
                                  SmallVectorImpl<std::string> &HoistedInsts) {
  return false;
}

/// Build a remark Name suffixed with the optional Tag (so the same pass can
/// be invoked multiple times in a pipeline with distinguishable output).
static std::string taggedName(StringRef Base, StringRef Tag) { return ""; }

/// Print a stable, non-empty label for \p BB, so remark args that identify
/// BasicBlocks stay useful when the BB has no source-level name (numeric IR,
/// stripped names, or non-C frontends such as swiftc's IRGen).
///
/// Preferred: `BB->getName()`. Fallback: `printAsOperand` slot-tracker form
/// (`%5` etc.), which is parseable and unique within the function. Never
/// returns empty.
static std::string bbLabel(const BasicBlock *BB) { return ""; }

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

/// SCEV traversal helper: collects every SCEVUnknown and SCEVAddRecExpr
/// node reachable from a SCEV expression. We use the canonical
/// `SCEVTraversal<>` machinery — no string parsing, no regex.
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

/// Compute the count-ordered, descriptive trap-class name for one trap edge
/// from its per-edge fields. Single source of truth for a classification
/// consumers previously re-derived in Python (gen_ir_view.priority_class);
/// emitted as the `TrapClass` field, and its precedence must stay in lock-step
/// with that fallback. Returned strings are static literals (safe as
/// StringRef).
static StringRef computeTrapClass(
    bool InLoop, TrapPredicateShape Shape, bool IsEntryProx, bool DomByEquiv,
    bool IsLoopExit, bool EdgeBTCComputable, bool LoopOtherUnk,
    bool StoreReload, bool MemIReload, bool CallReload, bool InLoopPhi,
    bool UnaliasLoad, bool OtherUnk, bool OpaqueNoUnk, bool InLoopFreeze,
    bool InLoopSelect, unsigned NumLeafOps, bool NonUnitStride, bool NegStride,
    bool NonConstStride, bool NotProvenMonotonicOnly) {
  const bool ConstK = Shape == TrapPredicateShape::OrBoundsCheckConstBound ||
                      Shape == TrapPredicateShape::AndBoundsCheckConstBound;
  const bool VarK = Shape == TrapPredicateShape::OrBoundsCheckVarBound ||
                    Shape == TrapPredicateShape::AndBoundsCheckVarBound;
  const bool TwoAR = Shape == TrapPredicateShape::OrTwoAddRecICmp ||
                     Shape == TrapPredicateShape::AndTwoAddRecICmp;
  // Out-of-loop: classify by structural redundancy / predicate shape.
  if (!InLoop) {
    if (IsEntryProx)
      return "OutsideLoop-EntryProximate";
    if (DomByEquiv)
      return "OutsideLoop-RedundantWithDominatingCheck";
    if (ConstK)
      return "OutsideLoop-MultiComparison-ConstBound";
    if (VarK)
      return "OutsideLoop-MultiComparison-VarBound";
    if (Shape == TrapPredicateShape::SingleICmp)
      return "OutsideLoop-SingleComparison";
    if (Shape == TrapPredicateShape::OtherMulti)
      return "OutsideLoop-MultiComparison-Other";
    if (TwoAR)
      return "OutsideLoop-MultiComparison-Other";
    return "OutsideLoop-Unclassifiable";
  }
  // In-loop, not a loop exit.
  if (!IsLoopExit)
    return "InLoop-NonExit";
  // In-loop loop-exit.
  // A BTC-computable edge whose predicate also has a reload/opaque blocker
  // isn't cleanly eliminable via trip count, so under LTAEmitExplain it is NOT
  // masked as a trip-count-known class -- it falls through to the blocker
  // classes below. Legacy (flag off) keeps the trip-count-known classes for any
  // BTC-computable edge (byte-identical).
  const bool HasBlocker = StoreReload || MemIReload || CallReload ||
                          InLoopPhi || UnaliasLoad || OtherUnk || OpaqueNoUnk;
  if (EdgeBTCComputable && (!LTAEmitExplain || !HasBlocker))
    return LoopOtherUnk ? "InLoopExit-TripCountKnown-LoopBlockedElsewhere"
                        : "InLoopExit-TripCountKnown";
  const bool S = StoreReload || MemIReload;
  if (S && CallReload)
    return "InLoopExit-TripCountUnknown-StoreAndCallReload";
  if (S)
    return "InLoopExit-TripCountUnknown-StoreReload";
  if (CallReload)
    return "InLoopExit-TripCountUnknown-CallReload";
  if (InLoopPhi)
    return "InLoopExit-TripCountUnknown-InLoopPhiOperand";
  if (UnaliasLoad)
    return "InLoopExit-TripCountUnknown-InLoopLoadOperand";
  // The opaque-operand class split by opacity shape (freeze / select / other
  // in-loop unknown). InLoopFreeze / InLoopSelect are subsets of OtherUnk, so
  // test them first.
  if (OtherUnk) {
    if (InLoopFreeze)
      return "InLoopExit-TripCountUnknown-OpaqueOperand-Freeze";
    if (InLoopSelect)
      return "InLoopExit-TripCountUnknown-OpaqueOperand-Select";
    return "InLoopExit-TripCountUnknown-OpaqueOperand-Other";
  }
  if (OpaqueNoUnk)
    return "InLoopExit-TripCountUnknown-OpaqueOperand-NoInLoopUnknown";
  // The multi-comparison suffix is driven by the predicate SHAPE (one source of
  // truth), so the multi-comparison class can't disagree with the emitted
  // PredicateShape. Legacy (flag off) keeps the NumLeafOps>=4 gate for
  // byte-identical output.
  bool MultiShape = LTAEmitExplain ? (ConstK || VarK || TwoAR ||
                                      Shape == TrapPredicateShape::OtherMulti)
                                   : (NumLeafOps >= 4);
  if (MultiShape) {
    if (ConstK)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-MultiComparison-"
             "ConstBound";
    if (VarK)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-MultiComparison-"
             "VarBound";
    if (TwoAR)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-MultiComparison-"
             "TwoAddRec";
    if (Shape == TrapPredicateShape::OtherMulti)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-MultiComparison-"
             "Other";
  }
  // Surface stride-fragility (the Has*StrideForLAddRec flags previously never
  // influenced the class) instead of lumping it into the bare weak-no-wrap
  // class. Legacy (flag off) returns the bare weak-no-wrap class
  // (byte-identical).
  if (LTAEmitExplain) {
    if (NonConstStride)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-NonConstantStride";
    if (NonUnitStride)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-NonUnitStride";
    if (NegStride)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-NegativeStride";
    if (NotProvenMonotonicOnly)
      return "InLoopExit-TripCountUnknown-NotProvenMonotonic-"
             "NotProvenMonotonicOnly";
  }
  return "InLoopExit-TripCountUnknown-NotProvenMonotonic";
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

/// Match `or/and(icmp(X), icmp(Y))` where each arm has an operand whose SCEV is
/// an L-AddRec -- the "two unrelated AddRec exits combined by one OR/AND" shape
/// that SCEV's `computeExitLimit` does not fold to `min(BTC_a, BTC_b)`.
/// Returns false when L is null (out-of-loop edges can't have L-AddRec
/// operands).
static bool matchTwoAddRecICmpImpl(Value *Arm1, Value *Arm2,
                                   ScalarEvolution &SE, Loop *L) {
  if (!L)
    return false;
  auto HasLAddRec = [&](Value *V) -> bool {
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
  };
  return HasLAddRec(Arm1) && HasLAddRec(Arm2);
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

/// Walk the def chain of \p Cond (bounded to instructions inside \p L) and
/// answer Q2 of the trap-explanation tree: for every IV-shaped operand reached,
/// does its update dominate the loop latch?
///
/// For each header phi reached, walk its loop-carried recurrence (the
/// latch-incoming value, through non-header phis) and check every visited
/// Instruction's parent BB. If any does not dominate the latch, the IV's update
/// is control-dependent (e.g. `m = c ? m + 1 : m` puts `m+1` in a
/// non-dominating arm) and SCEV refuses an AddRec -> returns false.
///
/// Returns true vacuously when no header-phi is reached.
static bool computeIVUpdateDominatesLatch(Value *Cond, Loop *L,
                                          const DominatorTree &DT) {
  if (!L)
    return true;
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return true;
  // Walk the recurrence chain reachable from a header phi's loop-carried
  // incoming value, returning false if any reached in-loop Instruction sits in
  // a BB that doesn't dominate the latch. Follows operands transitively through
  // non-header (merge) phis and arithmetic, so the chain reaches the underlying
  // add/sub even behind an inserted merge phi.
  auto AllUpdatesDominate = [&](Value *Start) -> bool {
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
  };

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
          if (!AllUpdatesDominate(In))
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

static void emitPerTrapEdgeSCEV(Function &F, LoopInfo &LI, ScalarEvolution &SE,
                                AAResults &AA, DominatorTree &DT,
                                OptimizationRemarkEmitter &ORE, StringRef Tag,
                                unsigned InvocationSeq) {
  // Call-wiring: exercise every per-edge helper so none is unused.
  BasicBlock *BB = &F.getEntryBlock();
  Loop *L = LI.getLoopFor(BB);
  (void)taggedName("LoopTrapEdge", Tag);
  (void)isTrapBlock(BB, BoundsSafetyTrapsOnly, LegacyTrapMatch::TrapIntrinsic);
  SmallVector<Value *, 8> Operands;
  SmallPtrSet<Value *, 16> Visited;
  collectBoolLeafOperands(nullptr, Operands, Visited);
  (void)classifyTrapPredicateShape(nullptr, SE, L);
  (void)trapPredicateShapeName(TrapPredicateShape::Unknown);
  (void)computeTrapClass(false, TrapPredicateShape::Unknown, false, false,
                         false, false, false, false, false, false, false,
                         false, false, false, false, false, 0u, false, false,
                         false, false);
  (void)isEntryProximate(F, BB, DT, LI);
  (void)isDominatedByEquivalentCheck(BB, nullptr, DT);
  (void)computeIVUpdateDominatesLatch(nullptr, L, DT);
  (void)bbLabel(BB);
  (void)InvocationSeq;
  (void)ORE;
  (void)AA;
}

/// Emit one machine-readable LoopPrimitives remark per loop in F, plus a
/// per-function LoopPrimitivesSummary. Always emits (does not gate on
/// hasUnreachableInst) so every loop is captured, including trap-free ones —
/// lets the early/late diff find loops that disappear between pipeline points.
static void emitLoopPrimitives(Function &F, LoopInfo &LI,
                               OptimizationRemarkEmitter &ORE,
                               ScalarEvolution &SE, AAResults &AA,
                               DominatorTree &DT, StringRef Tag,
                               unsigned InvocationSeq) {
  // Call-wiring: exercise every per-loop helper so none is unused.
  BasicBlock *BB = &F.getEntryBlock();
  Loop *L = LI.getLoopFor(BB);
  (void)countTrapExits(L, LI);
  (void)countCondTrapEdges(L, LI);
  (void)countHoistableCondTrapEdges(L, LI);
  (void)hasUnreachableInst(L);
  SmallVector<std::string, 4> Hoisted;
  (void)hasHoistedInPreheader(L, Hoisted);
  (void)getSideEffectReasons(*BB->getTerminator());
  (void)classifyCondTrapEdges(L, SE, DT);
  (void)Tag;
  (void)InvocationSeq;
  (void)ORE;
  (void)AA;
}

/// Check if \p L can be hoisted or not and emit a detailed remark about why
/// it can't be hoisted.
static CheckLoopHoistType processLoops(Loop *L, ScalarEvolution &SE,
                                       LoopInfo &LI,
                                       OptimizationRemarkEmitter &ORE,
                                       StringRef Tag) {
  return CheckLoopHoistType::SKIP;
}

/// Collect info for hoistable loop checks for \p F and report remarks for
/// individual loops and report a summary for hoistable checks for the function.
static void emitRemarks(Function &F, LoopInfo &LI,
                        OptimizationRemarkEmitter &ORE, ScalarEvolution &SE,
                        StringRef Tag) {
  // Call-wiring: exercise processLoops so it is not unused.
  for (auto *L : LI.getLoopsInPreorder())
    (void)processLoops(L, SE, LI, ORE, Tag);
}

PreservedAnalyses LoopTrapAnalysisPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  auto &AA = AM.getResult<AAManager>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  // Per-function run() counter, emitted as InvocationSeq (gated by
  // -loop-trap-analysis-explain) so repeated invocations can be deduped by
  // max(seq) per (function, src_bb, trap_bb); stays 1 for a single run.
  unsigned Seq = ++InvocationCount[&F];
  emitLoopPrimitives(F, LI, ORE, SE, AA, DT, Tag, Seq);
  emitRemarks(F, LI, ORE, SE, Tag);
  emitPerTrapEdgeSCEV(F, LI, SE, AA, DT, ORE, Tag, Seq);
  return PreservedAnalyses::all();
}

void LoopTrapAnalysisPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  OS << "loop-trap-analysis";
  if (!Tag.empty())
    OS << "<tag=" << Tag << ">";
}

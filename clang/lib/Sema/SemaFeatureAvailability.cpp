//===--- SemaFeatureAvailability.cpp - Availability attribute handling ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file processes the feature availability attribute.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallSet.h"
#include <utility>

using namespace clang;
using namespace sema;

class DiagnoseUnguardedFeatureAvailability
    : public RecursiveASTVisitor<DiagnoseUnguardedFeatureAvailability> {

  typedef RecursiveASTVisitor<DiagnoseUnguardedFeatureAvailability> Base;

  Sema &SemaRef;
  const Decl *D;

  struct FeatureAvailInfo {
    llvm::SmallSet<std::string, 8> Available, Unavailable;
    bool IsStack;
    void addString(const std::string &Name, bool IsUnavailable) {
      if (IsUnavailable)
        Unavailable.insert(Name);
      else
        Available.insert(Name);
    }
  };

  SmallVector<FeatureAvailInfo, 4> FeatureStack;

  bool isFeatureUseGuarded(const AvailabilityAttr *Attr) const;

  bool isConditionallyGuardedByFeature() const;

  void addFeaturesToStack(const Decl *D) {
    if (!D)
      return;
    do {
      for (auto *Attr : SemaRef.Context.getFeatureAvailabilityAttrs(D))
        FeatureStack.back().addString(Attr->getDomain().str(),
                                      Attr->getUnavailable());
    } while ((D = cast_or_null<Decl>(D->getDeclContext())));
  }

public:
  DiagnoseUnguardedFeatureAvailability(Sema &SemaRef, const Decl *D,
                                       Decl *Ctx = nullptr)
      : SemaRef(SemaRef), D(D) {
    FeatureStack.push_back({{}, {}, false});
    addFeaturesToStack(cast_or_null<Decl>(D->getDeclContext()));
    addFeaturesToStack(Ctx);
  }

  void diagnoseDeclFeatureAvailability(const NamedDecl *D, SourceLocation Loc);

  bool TraverseIfStmt(IfStmt *If);

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    diagnoseDeclFeatureAvailability(DRE->getDecl(), DRE->getBeginLoc());
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    diagnoseDeclFeatureAvailability(ME->getMemberDecl(), ME->getBeginLoc());
    return true;
  }

  bool VisitObjCMessageExpr(ObjCMessageExpr *OME) {
    diagnoseDeclFeatureAvailability(OME->getMethodDecl(), OME->getBeginLoc());
    return true;
  }

  bool VisitLabelStmt(LabelStmt *LS) {
    if (isConditionallyGuardedByFeature())
      SemaRef.Diag(LS->getBeginLoc(),
                   diag::err_label_in_conditionally_guarded_feature);
    return true;
  }

  bool VisitFunctionDecl(const FunctionDecl *FD) {
    pushDeclFeatures(FD);
    bool R = TraverseStmt(FD->getBody());
    FeatureStack.pop_back();
    return R;
  }

  bool VisitBlockDecl(const BlockDecl *BD) {
    pushDeclFeatures(BD);
    bool R = TraverseStmt(BD->getBody());
    FeatureStack.pop_back();
    return R;
  }

  bool VisitObjCMethodDecl(const ObjCMethodDecl *OMD) {
    pushDeclFeatures(OMD);
    bool R = TraverseStmt(OMD->getBody());
    FeatureStack.pop_back();
    return R;
  }

  // This is needed to avoid emit the diagnostic twice.
  bool WalkUpFromBlockDecl(BlockDecl *BD) { return true; }

  void pushDeclFeatures(const Decl *D) {
    FeatureStack.push_back({{}, {}, false});
    for (auto *Attr : SemaRef.Context.getFeatureAvailabilityAttrs(D))
      FeatureStack.back().addString(Attr->getDomain().str(),
                                    Attr->getUnavailable());
  }

  void IssueDiagnostics() {
    if (auto *FD = dyn_cast<FunctionDecl>(D))
      VisitFunctionDecl(FD);
    else if (auto *BD = dyn_cast<BlockDecl>(D))
      VisitBlockDecl(BD);
    else if (auto *OMD = dyn_cast<ObjCMethodDecl>(D))
      VisitObjCMethodDecl(OMD);
  }
};

struct ExtractedFeatureExpr {
  const ObjCAvailabilityCheckExpr *E = nullptr;
  bool isNegated = false;
};

static ExtractedFeatureExpr extractFeatureExpr(const Expr *IfCond) {
  const auto *E = IfCond;
  bool IsNegated = false;
  while (true) {
    E = E->IgnoreParens();
    if (const auto *AE = dyn_cast<ObjCAvailabilityCheckExpr>(E)) {
      if (AE->getDomainName().empty())
        return ExtractedFeatureExpr{};
      return ExtractedFeatureExpr{AE, IsNegated};
    }

    const auto *UO = dyn_cast<UnaryOperator>(E);
    if (!UO || UO->getOpcode() != UO_LNot) {
      return ExtractedFeatureExpr{};
    }
    E = UO->getSubExpr();
    IsNegated = !IsNegated;
  }
}

bool DiagnoseUnguardedFeatureAvailability::isConditionallyGuardedByFeature()
    const {
  return FeatureStack.back().IsStack;
}

bool DiagnoseUnguardedFeatureAvailability::TraverseIfStmt(IfStmt *If) {
  ExtractedFeatureExpr IfCond;
  if (auto *Cond = If->getCond())
    IfCond = extractFeatureExpr(Cond);
  if (!IfCond.E) {
    // This isn't an availability checking 'if', we can just continue.
    return Base::TraverseIfStmt(If);
  }

  std::string FeatureStr = IfCond.E->getDomainName().str();
  auto *Guarded = If->getThen();
  auto *Unguarded = If->getElse();
  if (IfCond.isNegated) {
    std::swap(Guarded, Unguarded);
  }

  FeatureStack.push_back({{}, {}, true});
  FeatureStack.back().addString(FeatureStr, false);
  bool ShouldContinue = TraverseStmt(Guarded);
  FeatureStack.pop_back();

  if (!ShouldContinue)
    return false;

  FeatureStack.push_back({{}, {}, true});
  FeatureStack.back().addString(FeatureStr, true);
  ShouldContinue = TraverseStmt(Unguarded);
  FeatureStack.pop_back();
  return ShouldContinue;
}

bool DiagnoseUnguardedFeatureAvailability::isFeatureUseGuarded(
    const AvailabilityAttr *Attr) const {
  for (auto &Features : FeatureStack) {
    auto Name = Attr->getDomain();
    if (Attr->getUnavailable()) {
      if (Features.Unavailable.count(Name.str()))
        return true;
    } else {
      if (Features.Available.count(Name.str()))
        return true;
    }
  }
  return false;
}

void DiagnoseUnguardedFeatureAvailability::diagnoseDeclFeatureAvailability(
    const NamedDecl *D, SourceLocation Loc) {
  SmallVector<AvailabilityAttr *, 2> AttrsOnDecl =
      SemaRef.Context.getFeatureAvailabilityAttrs(D);

  for (auto *Attr : AttrsOnDecl) {
    std::string FeatureUse = Attr->getDomain().str();
    if (!isFeatureUseGuarded(Attr))
      SemaRef.Diag(Loc, diag::err_unguarded_feature) << D << FeatureUse;
  }
}

static void DoEmitFeatureAvailabilityWarning(Sema &S, const NamedDecl *D,
                                             Decl *Ctx, SourceLocation Loc) {
  DiagnoseUnguardedFeatureAvailability Diag(S, D, Ctx);
  Diag.diagnoseDeclFeatureAvailability(D, Loc);
}

void Sema::handleDelayedFeatureAvailabilityCheck(DelayedDiagnostic &DD,
                                                 Decl *Ctx) {
  assert(DD.Kind == DelayedDiagnostic::FeatureAvailability &&
         "Expected a feature availability diagnostic here");

  DD.Triggered = true;
  DoEmitFeatureAvailabilityWarning(*this, DD.getFeatureAvailabilityDecl(), Ctx,
                                   DD.Loc);
}

void Sema::DiagnoseUnguardedFeatureAvailabilityViolations(Decl *D) {
  Stmt *Body = nullptr;

  if (auto *FD = D->getAsFunction()) {
    Body = FD->getBody();
  } else if (auto *MD = dyn_cast<ObjCMethodDecl>(D))
    Body = MD->getBody();
  else if (auto *BD = dyn_cast<BlockDecl>(D))
    Body = BD->getBody();

  assert(Body && "Need a body here!");

  DiagnoseUnguardedFeatureAvailability(*this, D).IssueDiagnostics();
}

void Sema::DiagnoseFeatureAvailabilityOfDecl(NamedDecl *D,
                                             ArrayRef<SourceLocation> Locs) {
  if (auto *AA = D->getAttr<AvailabilityAttr>())
    if (!AA->getDomain().empty())
      if (FunctionScopeInfo *Context = getCurFunctionAvailabilityContext()) {
        Context->HasPotentialFeatureAvailabilityViolations = true;
        return;
      }

  if (DelayedDiagnostics.shouldDelayDiagnostics()) {
    DelayedDiagnostics.add(DelayedDiagnostic::makeFeatureAvailability(D, Locs));
    return;
  }

  Decl *Ctx = cast<Decl>(getCurLexicalContext());
  DiagnoseUnguardedFeatureAvailability Diag(*this, D, Ctx);
  Diag.diagnoseDeclFeatureAvailability(D, Locs.front());
}

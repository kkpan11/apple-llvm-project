//===-- SemaBoundsSafety.cpp - Bounds Safety specific routines-*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares semantic analysis functions specific to `-fbounds-safety`
/// (Bounds Safety) and also its attributes when used without `-fbounds-safety`
/// (e.g. `counted_by`)
///
//===----------------------------------------------------------------------===//
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Sema.h"

namespace clang {

// In upstream the return type is `CountAttributedType::DynamicCountPointerKind`
/* TO_UPSTREAM(BoundsSafety) ON*/
CountAttributedType::BoundsAttrKind
/* TO_UPSTREAM(BoundsSafety) OFF*/
getCountAttrKind(bool CountInBytes, bool OrNull) {
  if (CountInBytes)
    return OrNull ? CountAttributedType::SizedByOrNull
                  : CountAttributedType::SizedBy;
  return OrNull ? CountAttributedType::CountedByOrNull
                : CountAttributedType::CountedBy;
}

static const RecordDecl *GetEnclosingNamedOrTopAnonRecord(const FieldDecl *FD,
                                                  // TO_UPSTREAM(BoundsSafety)
                                                          Sema &S) {
  const auto *RD = FD->getParent();
  // An unnamed struct is anonymous struct only if it's not instantiated.
  // However, the struct may not be fully processed yet to determine
  // whether it's anonymous or not. In that case, this function treats it as
  // an anonymous struct and tries to find a named parent.

  /* TO_UPSTREAM(BoundsSafety) ON*/
  const auto *ParentOfDeclWithAttr = FD->getParent();
  auto ShouldGetParent = [&]() -> bool {
    if (!S.getLangOpts().ExperimentalLateParseAttributes ||
        RD != ParentOfDeclWithAttr) {
      // This is the condition in upstream
      return (RD->isAnonymousStructOrUnion() ||
              (!RD->isCompleteDefinition() && RD->getName().empty()));
    }
    // In `Parser::ParseStructUnionBody` we have an Apple Internal change
    // to call `Actions.ActOnFields` **before** late parsed attributes are
    // semantically checked. In upstream `Action.ActOnFields` is called
    // afterwards. The effect of this is observable in this function because
    // `RD->isCompleteDefinition()` will return true for the struct we are
    // processing the attributes on with the Apple Internal change and false
    // in upstream.
    //
    // E.g.:
    //
    // struct on_pointer_anon_buf {
    // int count;
    //   struct {
    //     struct size_known *buf __counted_by(count);
    //   }; // <<-- Processing late parsed attrs of this struct
    // };
    //
    // For this particular example what's also counter-intuitive is that
    // `RD->isAnonymousStructOrUnion()` returns false for the anonymous
    // struct, that's because the `;` after the struct hasn't been processed
    // yet so it hasn't been marked as anonymous yet.

    // HACK:
    // To make lit tests work we don't test `RD->isCompleteDefinition()`
    // when it's the RecordDecl that contains the FieldDecl with a `counted_by`
    // like attribute (that we are in the middle of checking). Once we've gone
    // beyond that RecordDecl we traverse just like upstream clang does.
    //
    // TODO: Remove this hack once we upstream the `Actions.ActOnFields`
    // change (rdar://133402603).
    assert(RD == ParentOfDeclWithAttr);
    return RD->isAnonymousStructOrUnion() || RD->getName().empty();
  };
  /* TO_UPSTREAM(BoundsSafety) OFF*/

  while (RD && ShouldGetParent()) {
    const auto *Parent = dyn_cast<RecordDecl>(RD->getParent());
    if (!Parent)
      break;
    RD = Parent;
  }
  return RD;
}

enum class CountedByInvalidPointeeTypeKind {
  INCOMPLETE,
  SIZELESS,
  FUNCTION,
  FLEXIBLE_ARRAY_MEMBER,
  VALID,
};

bool Sema::CheckCountedByAttrOnField(FieldDecl *FD, Expr *E, bool CountInBytes,
                                     bool OrNull) {
  // Check the context the attribute is used in

  unsigned Kind = getCountAttrKind(CountInBytes, OrNull);

  if (FD->getParent()->isUnion()) {
    Diag(FD->getBeginLoc(), diag::err_count_attr_in_union)
        << Kind << FD->getSourceRange();
    return true;
  }

  const auto FieldTy = FD->getType();
  if (FieldTy->isArrayType() && (CountInBytes || OrNull)) {
    Diag(FD->getBeginLoc(),
         diag::err_count_attr_not_on_ptr_or_flexible_array_member)
        << Kind << FD->getLocation() << /* suggest counted_by */ 1;
    return true;
  }
  if (!FieldTy->isArrayType() && !FieldTy->isPointerType()) {
    Diag(FD->getBeginLoc(),
         diag::err_count_attr_not_on_ptr_or_flexible_array_member)
        << Kind << FD->getLocation() << /* do not suggest counted_by */ 0;
    return true;
  }

  LangOptions::StrictFlexArraysLevelKind StrictFlexArraysLevel =
      LangOptions::StrictFlexArraysLevelKind::IncompleteOnly;
  if (FieldTy->isArrayType() &&
      !Decl::isFlexibleArrayMemberLike(getASTContext(), FD, FieldTy,
                                       StrictFlexArraysLevel, true)) {
    Diag(FD->getBeginLoc(),
         diag::err_counted_by_attr_on_array_not_flexible_array_member)
        << Kind << FD->getLocation();
    return true;
  }

  CountedByInvalidPointeeTypeKind InvalidTypeKind =
      CountedByInvalidPointeeTypeKind::VALID;
  QualType PointeeTy;
  int SelectPtrOrArr = 0;
  if (FieldTy->isPointerType()) {
    PointeeTy = FieldTy->getPointeeType();
    SelectPtrOrArr = 0;
  } else {
    assert(FieldTy->isArrayType());
    const ArrayType *AT = getASTContext().getAsArrayType(FieldTy);
    PointeeTy = AT->getElementType();
    SelectPtrOrArr = 1;
  }
  // Note: The `Decl::isFlexibleArrayMemberLike` check earlier on means
  // only `PointeeTy->isStructureTypeWithFlexibleArrayMember()` is reachable
  // when `FieldTy->isArrayType()`.
  bool ShouldWarn = false;
  if (PointeeTy->isIncompleteType() && !CountInBytes) {
    InvalidTypeKind = CountedByInvalidPointeeTypeKind::INCOMPLETE;
  } else if (PointeeTy->isSizelessType()) {
    InvalidTypeKind = CountedByInvalidPointeeTypeKind::SIZELESS;
  } else if (PointeeTy->isFunctionType()) {
    InvalidTypeKind = CountedByInvalidPointeeTypeKind::FUNCTION;
  } else if (PointeeTy->isStructureTypeWithFlexibleArrayMember()) {
    if (FieldTy->isArrayType() && !getLangOpts().BoundsSafety) {
      // This is a workaround for the Linux kernel that has already adopted
      // `counted_by` on a FAM where the pointee is a struct with a FAM. This
      // should be an error because computing the bounds of the array cannot be
      // done correctly without manually traversing every struct object in the
      // array at runtime. To allow the code to be built this error is
      // downgraded to a warning.
      ShouldWarn = true;
    }
    InvalidTypeKind = CountedByInvalidPointeeTypeKind::FLEXIBLE_ARRAY_MEMBER;
  }

  if (InvalidTypeKind != CountedByInvalidPointeeTypeKind::VALID) {
    unsigned DiagID = ShouldWarn
                          ? diag::warn_counted_by_attr_elt_type_unknown_size
                          : diag::err_counted_by_attr_pointee_unknown_size;
    Diag(FD->getBeginLoc(), DiagID)
        << SelectPtrOrArr << PointeeTy << (int)InvalidTypeKind
        << (ShouldWarn ? 1 : 0) << Kind << FD->getSourceRange();
    return true;
  }

  // Check the expression

  if (!E->getType()->isIntegerType() || E->getType()->isBooleanType()) {
    Diag(E->getBeginLoc(), diag::err_count_attr_argument_not_integer)
        << Kind << E->getSourceRange();
    return true;
  }

  auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE) {
    Diag(E->getBeginLoc(),
         diag::err_count_attr_only_support_simple_decl_reference)
        << Kind << E->getSourceRange();
    return true;
  }

  auto *CountDecl = DRE->getDecl();
  FieldDecl *CountFD = dyn_cast<FieldDecl>(CountDecl);
  if (auto *IFD = dyn_cast<IndirectFieldDecl>(CountDecl)) {
    CountFD = IFD->getAnonField();
  }
  if (!CountFD) {
    Diag(E->getBeginLoc(), diag::err_count_attr_must_be_in_structure)
        << CountDecl << Kind << E->getSourceRange();

    Diag(CountDecl->getBeginLoc(),
         diag::note_flexible_array_counted_by_attr_field)
        << CountDecl << CountDecl->getSourceRange();
    return true;
  }

  if (FD->getParent() != CountFD->getParent()) {
    if (CountFD->getParent()->isUnion()) {
      Diag(CountFD->getBeginLoc(), diag::err_count_attr_refer_to_union)
          << Kind << CountFD->getSourceRange();
      return true;
    }
    // Whether CountRD is an anonymous struct is not determined at this
    // point. Thus, an additional diagnostic in case it's not anonymous struct
    // is done later in `Parser::ParseStructDeclaration`.
    /* TO_UPSTREAM(BoundsSafety) ON*/
    // Upstream doesn't pass `*this`.
    auto *RD = GetEnclosingNamedOrTopAnonRecord(FD, *this);
    auto *CountRD = GetEnclosingNamedOrTopAnonRecord(CountFD, *this);
    /* TO_UPSTREAM(BoundsSafety) OFF*/

    if (RD != CountRD) {
      Diag(E->getBeginLoc(), diag::err_count_attr_param_not_in_same_struct)
          << CountFD << Kind << FieldTy->isArrayType() << E->getSourceRange();
      Diag(CountFD->getBeginLoc(),
           diag::note_flexible_array_counted_by_attr_field)
          << CountFD << CountFD->getSourceRange();
      return true;
    }
  }
  return false;
}

/* TO_UPSTREAM(BoundsSafety) ON*/
static SourceRange SourceRangeFor(const CountAttributedType *CATy, Sema &S) {
  // Note: This implementation relies on `CountAttributedType` being unique.
  // E.g.:
  //
  // struct Foo {
  //   int count;
  //   char* __counted_by(count) buffer;
  //   char* __counted_by(count) buffer2;
  // };
  //
  // The types of `buffer` and `buffer2` are unique. The types being
  // unique means the SourceLocation of the `counted_by` expression can be used
  // to find where the attribute was written.

  auto Fallback = CATy->getCountExpr()->getSourceRange();
  auto CountExprBegin = CATy->getCountExpr()->getBeginLoc();

  // FIXME: We currently don't support the count expression being a macro
  // itself. E.g.:
  //
  // #define ZERO 0
  // int* __counted_by(ZERO) x;
  //
  if (S.SourceMgr.isMacroBodyExpansion(CountExprBegin))
    return Fallback;

  auto FetchIdentifierTokenFromOffset =
      [&](ssize_t Offset) -> std::optional<Token> {
    SourceLocation OffsetLoc = CountExprBegin.getLocWithOffset(Offset);
    Token Result;
    if (Lexer::getRawToken(OffsetLoc, Result, S.SourceMgr, S.getLangOpts()))
      return std::optional<Token>(); // Failed

    if (!Result.isAnyIdentifier())
      return std::optional<Token>(); // Failed

    return Result; // Success
  };

  auto CountExprEnd = CATy->getCountExpr()->getEndLoc();
  auto FindRParenTokenAfter = [&]() -> std::optional<Token> {
    auto CountExprEndSpelling = S.SourceMgr.getSpellingLoc(CountExprEnd);
    auto MaybeRParenTok = Lexer::findNextToken(
        CountExprEndSpelling, S.getSourceManager(), S.getLangOpts());

    if (!MaybeRParenTok.has_value())
      return std::nullopt;

    if (!MaybeRParenTok->is(tok::r_paren))
      return std::nullopt;

    return *MaybeRParenTok;
  };

  // Step back two characters to point at the last character of the attribute
  // text.
  //
  // __counted_by(count)
  //            ^ ^
  //            | |
  //            ---
  auto MaybeLastAttrCharToken = FetchIdentifierTokenFromOffset(-2);
  if (!MaybeLastAttrCharToken)
    return Fallback;

  auto LastAttrCharToken = MaybeLastAttrCharToken.value();

  if (LastAttrCharToken.getLength() > 1) {
    // Special case: When the character is part of a macro the Token we get
    // is the whole macro name (e.g. `__counted_by`).
    if (LastAttrCharToken.getRawIdentifier() !=
        CATy->GetAttributeName(/*WithMacroPrefix=*/true))
      return Fallback;

    // Found the beginning of the `__counted_by` macro
    SourceLocation Begin = LastAttrCharToken.getLocation();
    // Now try to find the closing `)` of the macro.
    auto MaybeRParenTok = FindRParenTokenAfter();
    if (!MaybeRParenTok.has_value())
      return Fallback;

    return SourceRange(Begin, MaybeRParenTok->getLocation());
  }

  assert(LastAttrCharToken.getLength() == 1);
  // The Token we got back is just the last character of the identifier.
  // This means a macro is not being used and instead the attribute is being
  // used directly. We need to find the beginning of the identifier. We support
  // two cases:
  //
  // * Non-affixed version. E.g: `counted_by`
  // * Affixed version. E.g.: `__counted_by__`

  // Try non-affixed version. E.g.:
  //
  // __attribute__((counted_by(count)))
  //                ^          ^
  //                |          |
  //                ------------

  // +1 is for `(`
  const ssize_t NonAffixedSkipCount =
      CATy->GetAttributeName(/*WithMacroPrefix=*/false).size() + 1;
  auto MaybeNonAffixedBeginToken =
      FetchIdentifierTokenFromOffset(-NonAffixedSkipCount);
  if (!MaybeNonAffixedBeginToken)
    return Fallback;

  auto NonAffixedBeginToken = MaybeNonAffixedBeginToken.value();
  if (NonAffixedBeginToken.getRawIdentifier() ==
      CATy->GetAttributeName(/*WithMacroPrefix=*/false)) {
    // Found the beginning of the `counted_by`-like attribute
    auto SL = NonAffixedBeginToken.getLocation();

    // Now try to find the closing `)` of the attribute
    auto MaybeRParenTok = FindRParenTokenAfter();
    if (!MaybeRParenTok.has_value())
      return Fallback;

    return SourceRange(SL, MaybeRParenTok->getLocation());
  }

  // Try affixed version. E.g.:
  //
  // __attribute__((__counted_by__(count)))
  //                ^              ^
  //                |              |
  //                ----------------
  std::string AffixedTokenStr =
      (llvm::Twine("__") + CATy->GetAttributeName(/*WithMacroPrefix=*/false) +
       llvm::Twine("__"))
          .str();
  // +1 is for `(`
  // +4 is for the 4 `_` characters
  const ssize_t AffixedSkipCount =
      CATy->GetAttributeName(/*WithMacroPrefix=*/false).size() + 1 + 4;
  auto MaybeAffixedBeginToken =
      FetchIdentifierTokenFromOffset(-AffixedSkipCount);
  if (!MaybeAffixedBeginToken)
    return Fallback;

  auto AffixedBeginToken = MaybeAffixedBeginToken.value();
  if (AffixedBeginToken.getRawIdentifier() != AffixedTokenStr)
    return Fallback;

  // Found the beginning of the `__counted_by__`-like like attribute.
  auto SL = AffixedBeginToken.getLocation();
  // Now try to find the closing `)` of the attribute
  auto MaybeRParenTok = FindRParenTokenAfter();
  if (!MaybeRParenTok.has_value())
    return Fallback;

  return SourceRange(SL, MaybeRParenTok->getLocation());
}

static void EmitIncompleteCountedByPointeeNotes(Sema &S,
                                                const CountAttributedType *CATy,
                                                NamedDecl *IncompleteTyDecl,
                                                bool NoteAttrLocation = true) {
  assert(IncompleteTyDecl == nullptr || isa<TypeDecl>(IncompleteTyDecl));

  if (NoteAttrLocation) {
    // Note where the attribute is declared
    auto AttrSrcRange = SourceRangeFor(CATy, S);
    S.Diag(AttrSrcRange.getBegin(), diag::note_named_attribute)
        << CATy->GetAttributeName(/*WithMacroPrefix=*/true) << AttrSrcRange;
  }

  if (!IncompleteTyDecl)
    return;

  // If there's an associated forward declaration display it to emphasize
  // why the type is incomplete (all we have is a forward declaration).

  // Note the `IncompleteTyDecl` type is the underlying type which might not
  // be the same as `CATy->getPointeeType()` which could be a typedef.
  //
  // The diagnostic printed will be at the location of the underlying type but
  // the diagnostic text will print the type of `CATy->getPointeeType()` which
  // could be a typedef name rather than the underlying type. This is ok
  // though because the diagnostic will print the underlying type name too.
  // E.g:
  //
  // `forward declaration of 'Incomplete_Struct_t'
  //  (aka 'struct IncompleteStructTy')`
  //
  // If this ends up being confusing we could emit a second diagnostic (one
  // explaining where the typedef is) but that seems overly verbose.

  S.Diag(IncompleteTyDecl->getBeginLoc(), diag::note_forward_declaration)
      << CATy->getPointeeType();
}

static bool
HasCountedByAttrOnIncompletePointee(QualType Ty, NamedDecl **ND,
                                    const CountAttributedType **CATyOut,
                                    QualType *PointeeTyOut) {
  auto *CATy = Ty->getAs<CountAttributedType>();
  if (!CATy)
    return false;

  // Incomplete pointee type is only a problem for
  // counted_by/counted_by_or_null
  if (CATy->isCountInBytes())
    return false;

  auto PointeeTy = CATy->getPointeeType();
  if (PointeeTy.isNull())
    return false; // Reachable?

  if (!PointeeTy->isIncompleteType(ND))
    return false;

  if (CATyOut)
    *CATyOut = CATy;
  if (PointeeTyOut)
    *PointeeTyOut = PointeeTy;
  return true;
}

bool Sema::BoundsSafetyCheckAssignmentToCountAttrPtrWithIncompletePointeeTy(
    QualType LHSTy, Expr *RHSExpr, AssignmentAction Action, SourceLocation Loc,
    const ValueDecl *Assignee, bool ShowFullyQualifiedAssigneeName) {
  NamedDecl *IncompleteTyDecl = nullptr;
  const CountAttributedType *CATy = nullptr;
  QualType PointeeTy;
  if (!HasCountedByAttrOnIncompletePointee(LHSTy, &IncompleteTyDecl, &CATy,
                                           &PointeeTy))
    return true;
  assert(CATy && !CATy->isCountInBytes() && !PointeeTy.isNull());

  // It's not expected that the diagnostic be emitted in these cases.
  // It's not necessarily a problem but we should catch when this starts
  // to happen.
  assert(Action != AssignmentAction::Converting &&
         Action != AssignmentAction::Sending &&
         Action != AssignmentAction::Casting &&
         Action != AssignmentAction::Passing_CFAudited);

  std::string AssigneeStr;
  if (Assignee) {
    if (ShowFullyQualifiedAssigneeName) {
      AssigneeStr = Assignee->getQualifiedNameAsString();
    } else {
      AssigneeStr = Assignee->getNameAsString();
    }
  }
  {
    auto D =
        Diag(Loc,
             diag::err_bounds_safety_counted_by_on_incomplete_type_on_assign)
        << /*0*/ (int)Action << /*1*/ AssigneeStr
        << /*2*/ (AssigneeStr.size() > 0)
        << /*3*/ isa<ImplicitValueInitExpr>(RHSExpr) << /*4*/ LHSTy
        << /*5*/ CATy->GetAttributeName(/*WithMacroPrefix=*/true)
        << /*6*/ PointeeTy << /*7*/ CATy->isOrNull();

    if (RHSExpr->getSourceRange().isValid())
      D << RHSExpr->getSourceRange();
  }

  EmitIncompleteCountedByPointeeNotes(*this, CATy, IncompleteTyDecl);
  return false; // check failed
}

bool Sema::BoundsSafetyCheckAssignmentToCountAttrPtr(
    QualType LHSTy, Expr *RHSExpr, AssignmentAction Action, SourceLocation Loc,
    const ValueDecl *Assignee, bool ShowFullyQualifiedAssigneeName) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  return BoundsSafetyCheckAssignmentToCountAttrPtrWithIncompletePointeeTy(
      LHSTy, RHSExpr, Action, Loc, Assignee, ShowFullyQualifiedAssigneeName);
}

bool Sema::BoundsSafetyCheckInitialization(const InitializedEntity &Entity,
                                           const InitializationKind &Kind,
                                           AssignmentAction Action,
                                           QualType LHSType, Expr *RHSExpr) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  bool ChecksPassed = true;
  auto SL = Kind.getLocation();

  // Note: We don't call `BoundsSafetyCheckAssignmentToCountAttrPtr` here
  // because we need conditionalize what is checked.

  // counted_by/counted_by_or_null on incomplete pointee type check.
  //
  // We skip Variable initializers because we should have already complained
  // about those variables in `Sema::BoundsSafetyCheckVarDecl()`.
  if (Action == AssignmentAction::Initializing &&
      Entity.getKind() != InitializedEntity::EK_Variable) {

    if (!BoundsSafetyCheckAssignmentToCountAttrPtrWithIncompletePointeeTy(
            LHSType, RHSExpr, Action, SL,
            dyn_cast_or_null<ValueDecl>(Entity.getDecl()),
            /*ShowFullQualifiedAssigneeName=*/true)) {

      ChecksPassed = false;

      // It's not necessarily bad if this assert fails but we should catch
      // if this happens.
      assert(Entity.getKind() == InitializedEntity::EK_Member);
    }
  }

  return ChecksPassed;
}

static bool BoundsSafetyCheckUseOfCountAttrPtrWithIncompletePointeeTy(Sema &S,
                                                                      Expr *E) {
  QualType T = E->getType();
  assert(T->isPointerType());

  // Generate a string for the diagnostic that describes the "use".
  // The string is specialized for direct calls to produce a better
  // diagnostic.
  StringRef DirectCallFn;
  std::string UseStr;
  if (const auto *CE = dyn_cast<CallExpr>(E->IgnoreParens())) {
    if (const auto *FD = CE->getDirectCallee()) {
      DirectCallFn = FD->getName();
    }
  }
  int SelectExprKind = DirectCallFn.size() > 0 ? 1 : 0;
  if (SelectExprKind) {
    UseStr = DirectCallFn;
  } else {
    llvm::raw_string_ostream SS(UseStr);
    E->printPretty(SS, nullptr, PrintingPolicy(S.getPrintingPolicy()));
  }
  assert(UseStr.size() > 0);

  const CountAttributedType *CATy = nullptr;
  QualType PointeeTy;
  NamedDecl *IncompleteTyDecl = nullptr;
  if (!HasCountedByAttrOnIncompletePointee(T, &IncompleteTyDecl, &CATy,
                                           &PointeeTy))
    return true;
  assert(CATy && !CATy->isCountInBytes() && !PointeeTy.isNull());

  S.Diag(E->getBeginLoc(),
         diag::err_bounds_safety_counted_by_on_incomplete_type_on_use)
      << /*0*/ SelectExprKind << /*1*/ UseStr << /*2*/ T << /*3*/ PointeeTy
      << /*4*/ CATy->GetAttributeName(/*WithMacroPrefix=*/true)
      << /*5*/ CATy->isOrNull() << E->getSourceRange();

  EmitIncompleteCountedByPointeeNotes(S, CATy, IncompleteTyDecl);
  return false;
}

bool Sema::BoundsSafetyCheckUseOfCountAttrPtr(Expr *E) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  QualType T = E->getType();
  if (!T->isPointerType())
    return true;

  return BoundsSafetyCheckUseOfCountAttrPtrWithIncompletePointeeTy(*this, E);
}

bool Sema::BoundsSafetyCheckResolvedCall(FunctionDecl *FDecl, CallExpr *Call,
                                         const FunctionProtoType *ProtoType) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  assert(Call);
  if (Call->containsErrors())
    return false;

  // Report incomplete pointee types on `__counted_by(__or_null)` pointers.
  bool ChecksPassed = true;

  // Check the return of the call. The call is treated as a "use" of
  // the return type.
  if (!BoundsSafetyCheckUseOfCountAttrPtr(Call))
    ChecksPassed = false;

  // Check parameters
  if (!FDecl && !ProtoType)
    return ChecksPassed; // Can't check any further so return early

  unsigned MinNumArgs =
      std::min(Call->getNumArgs(),
               FDecl ? FDecl->getNumParams() : ProtoType->getNumParams());

  for (size_t ArgIdx = 0; ArgIdx < MinNumArgs; ++ArgIdx) {
    Expr *CallArg = Call->getArg(ArgIdx); // FIXME: IgnoreImpCast()?
    const ValueDecl *ParamVarDecl = nullptr;
    QualType ParamTy;
    if (FDecl) {
      // Direct call
      ParamVarDecl = FDecl->getParamDecl(ArgIdx);
      ParamTy = ParamVarDecl->getType();
    } else {
      // Indirect call. The parameter name isn't known
      ParamTy = ProtoType->getParamType(ArgIdx);
    }

    // Assigning to the parameter type is treated as a "use" of the type.
    if (!BoundsSafetyCheckAssignmentToCountAttrPtr(
            ParamTy, CallArg, AssignmentAction::Passing, CallArg->getBeginLoc(),
            ParamVarDecl, /*ShowFullQualifiedAssigneeName=*/false))
      ChecksPassed = false;
  }
  return ChecksPassed;
}

static bool BoundsSafetyCheckFunctionParamOrCountAttrWithIncompletePointeeTy(
    Sema &S, QualType Ty, const ParmVarDecl *ParamDecl) {
  const CountAttributedType *CATy = nullptr;
  QualType PointeeTy;
  NamedDecl *IncompleteTyDecl = nullptr;
  if (!HasCountedByAttrOnIncompletePointee(Ty, &IncompleteTyDecl, &CATy,
                                           &PointeeTy))
    return true;

  // Emit Diagnostic
  StringRef ParamName;
  if (ParamDecl)
    ParamName = ParamDecl->getName();

  auto SR = SourceRangeFor(CATy, S);
  S.Diag(SR.getBegin(),
         diag::err_bounds_safety_counted_by_on_incomplete_type_on_func_def)
      << /*0*/ CATy->GetAttributeName(/*WithMacroPrefix*/ true)
      << /*1*/ (ParamDecl ? 1 : 0) << /*2*/ (ParamName.size() > 0)
      << /*3*/ ParamName << /*4*/ Ty << /*5*/ PointeeTy
      << /*6*/ CATy->isOrNull() << SR;

  EmitIncompleteCountedByPointeeNotes(S, CATy, IncompleteTyDecl,
                                      /*NoteAttrLocation=*/false);
  return false;
}

bool Sema::BoundsSafetyCheckParamForFunctionDef(const ParmVarDecl *PVD) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  return BoundsSafetyCheckFunctionParamOrCountAttrWithIncompletePointeeTy(
      *this, PVD->getType(), PVD);
}

bool Sema::BoundsSafetyCheckReturnTyForFunctionDef(FunctionDecl *FD) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  return BoundsSafetyCheckFunctionParamOrCountAttrWithIncompletePointeeTy(
      *this, FD->getReturnType(), nullptr);
}

static bool
BoundsSafetyCheckVarDeclCountAttrPtrWithIncompletePointeeTy(Sema &S,
                                                            const VarDecl *VD) {
  const CountAttributedType *CATy = nullptr;
  QualType PointeeTy;
  NamedDecl *IncompleteTyDecl = nullptr;
  if (!HasCountedByAttrOnIncompletePointee(VD->getType(), &IncompleteTyDecl,
                                           &CATy, &PointeeTy))
    return true;

  SourceRange SR = SourceRangeFor(CATy, S);
  S.Diag(SR.getBegin(),
         diag::err_bounds_safety_counted_by_on_incomplete_type_on_var_decl)
      << /*0*/ CATy->GetAttributeName(/*WithMacroPrefix=*/true)
      << /*1*/ (VD->isThisDeclarationADefinition() ==
                VarDecl::TentativeDefinition)
      << /*2*/ VD->getName() << /*3*/ VD->getType() << /*4*/ PointeeTy
      << /*5*/ CATy->isOrNull() << SR;

  EmitIncompleteCountedByPointeeNotes(S, CATy, IncompleteTyDecl,
                                      /*NoteAttrLocation=*/false);
  return false;
}

bool Sema::BoundsSafetyCheckVarDecl(const VarDecl *VD,
                                    bool CheckTentativeDefinitions) {
  if (!getLangOpts().hasBoundsSafety())
    return true;

  switch (VD->isThisDeclarationADefinition()) {
  case VarDecl::DeclarationOnly:
    // Using `__counted_by` on a pointer type with an incomplete pointee
    // isn't considered an error for declarations.
    return true;
  case VarDecl::TentativeDefinition:
    // A tentative definition may become an actual definition but this isn't
    // known until the end of the translation unit.
    // See `Sema::ActOnEndOfTranslationUnit()`
    if (!CheckTentativeDefinitions)
      return true;
    LLVM_FALLTHROUGH;
  case VarDecl::Definition:
    // Using `__counted_by` on a pointer type with an incomplete pointee
    // is considered an error for **definitions** so carry-on with checking.
    break;
  }

  return BoundsSafetyCheckVarDeclCountAttrPtrWithIncompletePointeeTy(*this, VD);
}

bool Sema::BoundsSafetyCheckCountAttributedTypeHasConstantCountForAssignmentOp(
    const CountAttributedType *CATTy, Expr *Operand,
    std::variant<bool, BinaryOperatorKind> OpInfo) {

  bool IsUnaryOp = std::holds_alternative<bool>(OpInfo);
  int SelectOp = 0;
  unsigned DiagID = 0;
  if (IsUnaryOp) {
    SelectOp = std::get<bool>(OpInfo);
    DiagID =
        diag::warn_bounds_safety_count_attr_pointer_unary_arithmetic_constant_count;
  } else {
    // Binary operator
    DiagID =
        diag::warn_bounds_safety_count_attr_pointer_binary_assign_constant_count;
    switch (std::get<BinaryOperatorKind>(OpInfo)) {
    case BO_AddAssign: // +=
      SelectOp = 1;
      break;
    case BO_SubAssign: // -=
      SelectOp = 0;
      break;
    default:
      // We shouldn't go down this path. Other operations on a
      // CountAttributedType pointers have the pointer promoted to a
      // __bidi_indexable first rather than keeping the CountAttributedType
      // type.
      llvm_unreachable("Unexpected BinaryOperatorKind");
      return true;
    }
  }

  Expr::EvalResult Result;
  if (CATTy->getCountExpr()->EvaluateAsInt(Result, getASTContext())) {
    // Count is constant
    Diag(Operand->getExprLoc(), DiagID) <<
        /*0*/ SelectOp <<
        /*1*/ CATTy->GetAttributeName(/*WithMacroPrefix=*/true) <<
        /*2*/ (CATTy->isCountInBytes() ? 1 : 0) <<
        /*3*/ 0 /* integer constant count*/ <<
        /*4*/ Result.Val.getAsString(getASTContext(),
                                     CATTy->getCountExpr()->getType());
    Diag(CATTy->getCountExpr()->getExprLoc(), diag::note_named_attribute)
        << CATTy->GetAttributeName(/*WithMacroPrefix=*/true);
    return false;
  }
  if (const auto *DRE =
          dyn_cast<DeclRefExpr>(CATTy->getCountExpr()->IgnoreParenCasts())) {
    const auto *VD = DRE->getDecl();
    if (VD->getType().isConstQualified()) {
      // Count expression refers to a single decl that is `const` qualified
      // which means it is effectively constant.

      Diag(Operand->getExprLoc(), DiagID) <<
          /*0*/ SelectOp <<
          /*1*/ CATTy->GetAttributeName(/*WithMacroPrefix=*/true) <<
          /*2*/ (CATTy->isCountInBytes() ? 1 : 0) <<
          /*3*/ 1 /* const qualified declref*/ <<
          /*4*/ VD;
      Diag(CATTy->getCountExpr()->getExprLoc(), diag::note_named_attribute)
          << CATTy->GetAttributeName(/*WithMacroPrefix=*/true);
      return false;
    }
  }

  return true;
}
/* TO_UPSTREAM(BoundsSafety) OFF*/

} // namespace clang

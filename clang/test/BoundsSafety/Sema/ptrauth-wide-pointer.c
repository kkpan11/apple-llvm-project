
// RUN: %clang_cc1 -triple arm64e -fptrauth-intrinsics -fbounds-safety -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple arm64e -fptrauth-intrinsics -fbounds-safety -x objective-c -fexperimental-bounds-safety-objc -fsyntax-only -verify %s

// rdar://182733053
//
// A wide pointer (__indexable / __bidi_indexable) passed to a ptrauth builtin
// is now converted to a raw-layout pointer, because a ptrauth operation acts on
// the raw address and the bounds are meaningless to it. Previously no conversion
// happened at all and the aggregate reached CodeGen, tripping EmitScalarExpr().
//
// This test pins down the Sema-visible consequence. For strip/auth/sign/resign
// the result type is taken from the value operand
// (Call->setType(Call->getArgs()[0]->getType())), and that read happens *after*
// the conversion -- so the result of these builtins on a wide pointer is a
// raw-layout pointer, and its bounds cannot silently come back. Feeding it to
// something that requires bounds is a hard error directing the user to a forge,
// which is what keeps the bounds loss explicit rather than silent.

#include <ptrcheck.h>

unsigned long a[10];

// Discriminator slots produce an integer, so a wide pointer is accepted with no
// diagnostic at all -- the bounds are simply not part of the result.
unsigned long blend_ok(unsigned long *__bidi_indexable p) {
  return __builtin_ptrauth_blend_discriminator(p, 42); // no diagnostic
}

unsigned long blend_addrof_ok(void) {
  return __builtin_ptrauth_blend_discriminator(&a[0], 42); // no diagnostic
}

unsigned long sign_generic_ok(unsigned long *__bidi_indexable p) {
  return __builtin_ptrauth_sign_generic_data(p, &a[1]); // no diagnostic
}

unsigned long auth_wide_disc_ok(unsigned long *__single p) {
  return (unsigned long)__builtin_ptrauth_auth(p, 2, &a[1]); // no diagnostic
}

// The value slot: the result is raw-layout, so it can be consumed anywhere a
// bound is not required.
unsigned long *__unsafe_indexable strip_to_unsafe(unsigned long *__bidi_indexable p) {
  return __builtin_ptrauth_strip(p, 2); // no diagnostic
}

unsigned long strip_to_integer(unsigned long *__bidi_indexable p) {
  return (unsigned long)__builtin_ptrauth_strip(p, 2); // no diagnostic
}

// ... but bounds cannot be re-acquired implicitly. This is the property that
// keeps a wide pointer from silently losing its bounds across a ptrauth call.
// (The result type is the same unspecified `unsigned long *` in both cases
// below -- verified via -ast-dump. Clang's type printer just spells it
// '__unsafe_indexable' when disambiguating against a __bidi_indexable
// destination and plainly against a __single one.)
void strip_to_bidi(unsigned long *__bidi_indexable p) {
  unsigned long *__bidi_indexable q = __builtin_ptrauth_strip(p, 2);
  // expected-error@-1{{initializing 'unsigned long *__bidi_indexable' with an expression of incompatible type 'unsigned long *__unsafe_indexable' casts away '__unsafe_indexable' qualifier; use '__unsafe_forge_single' or '__unsafe_forge_bidi_indexable' to perform this conversion}}
  (void)q;
}

void sign_to_single(unsigned long *__bidi_indexable p) {
  unsigned long *__single q = __builtin_ptrauth_sign_unauthenticated(p, 2, 0);
  // expected-error@-1{{initializing 'unsigned long *__single' with an expression of incompatible type 'unsigned long *' casts away '__unsafe_indexable' qualifier; use '__unsafe_forge_single' or '__unsafe_forge_bidi_indexable' to perform this conversion}}
  (void)q;
}

// A __single operand is raw-layout already, so its result stays __single and
// this remains valid -- the change must not disturb existing code.
unsigned long *__single strip_single_ok(unsigned long *__single p) {
  return __builtin_ptrauth_strip(p, 2); // no diagnostic
}

// __counted_by keeps its bounds outside the pointer, so it is unaffected too.
unsigned long counted_by_ok(unsigned long *__counted_by(n) p, int n) {
  return __builtin_ptrauth_blend_discriminator(p, 42); // no diagnostic
}

// __builtin_ptrauth_sign_constant reaches the same conversion (it is checked by
// PointerAuthSignOrAuth with RequireConstant=true), so a wide-pointer operand is
// converted here too. Before the fix this crashed in the constant-emission path
// with "Invalid constantexpr bitcast!" rather than in EmitScalarExpr -- a
// different symptom of the same missing cast, not a separate bug.
void *__unsafe_indexable const sign_const_ok =
    __builtin_ptrauth_sign_constant(&a[0], 2, 0); // no diagnostic

void *const sign_const_to_single = __builtin_ptrauth_sign_constant(&a[0], 2, 0);
// expected-error@-1{{initializing 'void *__singleconst' with an expression of incompatible type 'unsigned long *' casts away '__unsafe_indexable' qualifier; use '__unsafe_forge_single' or '__unsafe_forge_bidi_indexable' to perform this conversion}}

// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -verify=none %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe -verify=user %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-forges -verify=user %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-sdk -verify=sdk %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-forges-sdk -verify=sdk %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe -Wbounds-safety-unsafe-sdk -verify=user,sdk %s

// none-no-diagnostics
#include <ptrcheck.h>
#include "unsafe-forges-system.h"

void foo(int *p, char *s, unsigned n) {
  int *a = __unsafe_forge_single(int *, p);                        // user-warning{{use of '__unsafe_forge_single' is unsafe}}
  char *__null_terminated b =__unsafe_forge_terminated_by(char *, s, 0);            // user-warning{{use of '__unsafe_forge_terminated_by' is unsafe}}
  char *__null_terminated c =__unsafe_forge_null_terminated(char *, s);             // user-warning{{use of '__unsafe_forge_terminated_by' is unsafe}}
  void *d = __unsafe_forge_bidi_indexable(void *, p, n);           // user-warning{{use of '__unsafe_forge_bidi_indexable' is unsafe}}
  int *e = (int *)__builtin_unsafe_forge_single(p);                // user-warning{{use of '__unsafe_forge_single' is unsafe}}
}

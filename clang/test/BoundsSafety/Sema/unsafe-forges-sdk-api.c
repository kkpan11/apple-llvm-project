// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -verify=none %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe -verify=user %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-forges -verify=user %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-sdk -verify=none %s
// RUN: %clang_cc1 -fsyntax-only -fbounds-safety -Wbounds-safety-unsafe-forges-sdk -verify=none %s

// none-no-diagnostics
#include <ptrcheck.h>
#include "unsafe-forges-sdk-api.h"

// Forge is necessary to get a safe pointer from the __unsafe_indexable pointer
// returned by the SDK.
void foo(void) {
  int *local_int = __unsafe_forge_single(int *, getInt()); // user-warning{{use of '__unsafe_forge_single' is unsafe}}
}

void print_version_terminated_by(void) {
  const char *__terminated_by(0) version =
      __unsafe_forge_terminated_by(const char *, get_version(), 0); // user-warning{{use of '__unsafe_forge_terminated_by' is unsafe}}
}

void print_version_null_terminated(void) {
  const char *__null_terminated version =
      __unsafe_forge_null_terminated(const char *, get_version()); // user-warning{{use of '__unsafe_forge_terminated_by' is unsafe}}
}

void alloc_and_zero(void) {
  const size_t alloc_size = sizeof(int) * 2;
  int *data = (int *)__unsafe_forge_bidi_indexable( // user-warning{{use of '__unsafe_forge_bidi_indexable' is unsafe}}
      void *, alloc(alloc_size), alloc_size);
  __builtin_memset(data, 0, alloc_size);
}

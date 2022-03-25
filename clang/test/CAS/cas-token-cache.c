// Test running -fcas-token-cache.
//
// TODO: Removing the dependency on driver will be great (-fdepscan should get
// coverage somewhere else instead).

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=inline -Xclang -fcas-path -Xclang %t/cas -Xclang -fcas-token-cache -fsyntax-only -x c %s

#include "test.h"

int func(void);

// Test running -fdepscan.
//
// TODO: Test should be updated to not depend on a working cache at default
// location, which is out of test/build directory.
// REQUIRES: system-darwin

// RUN: rm -rf %t-*.d
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=daemon \
// RUN:   -E -MD -MF %t-daemon.d -x c %s >/dev/null
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=inline \
// RUN:   -E -MD -MF %t-inline.d -x c %s >/dev/null
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=auto \
// RUN:   -E -MD -MF %t-auto.d -x c %s >/dev/null
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=off \
// RUN:   -E -MD -MF %t-off.d -x c %s >/dev/null
//
// Check -fdepscan-share-related arguments are claimed.
// TODO: Check behaviour.
//
// RUN: %clang -target x86_64-apple-macos11 -I %S/Inputs -fdepscan=off \
// RUN:     -fdepscan-share-parent                                     \
// RUN:     -fdepscan-share-parent=                                    \
// RUN:     -fdepscan-share-parent=python                              \
// RUN:     -fdepscan-share=python                                     \
// RUN:     -fdepscan-share=                                           \
// RUN:     -fdepscan-share-stop=python                                \
// RUN:     -fno-depscan-share                                         \
// RUN:     -fsyntax-only -x c %s                                      \
// RUN: | FileCheck %s -allow-empty
// CHECK-NOT: warning:
//
// RUN: not %clang -target x86_64-apple-macos11 -I %S/Inputs \
// RUN:     -fdepscan-share-parents 2>&1                     \
// RUN: | FileCheck %s -check-prefix=BAD-SPELLING
// BAD-SPELLING: error: unknown argument '-fdepscan-share-parents'
//
// Check that the dependency files match.
// RUN: diff %t-off.d %t-daemon.d
// RUN: diff %t-off.d %t-inline.d
// RUN: diff %t-off.d %t-auto.d

#include "test.h"

int func(void);

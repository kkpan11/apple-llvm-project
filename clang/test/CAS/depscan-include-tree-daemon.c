// REQUIRES: clang-cc1daemon

// RUN: rm -rf %t
// RUN: split-file %s %t

// RUN: %clang -cc1depscan -o %t/inline.rsp -dump-depscan-tree=%t/inline.root -fdepscan=inline -cc1-args -cc1 -triple x86_64-apple-macos11.0 \
// RUN:     -fsyntax-only %t/t.c -I %t/includes -isysroot %S/Inputs/SDK -fcas-path %t/cas -DSOME_MACRO -dependency-file %t/inline.d -MT deps

// RUN: %clang -cc1depscand -execute %{clang-daemon-dir}/%basename_t -cas-args -fcas-path %t/cas -- \
// RUN:   %clang -cc1depscan -o %t/daemon.rsp -dump-depscan-tree=%t/daemon.root -fdepscan=daemon -fdepscan-daemon=%{clang-daemon-dir}/%basename_t \
// RUN:     -cc1-args -cc1 -triple x86_64-apple-macos11.0 \
// RUN:     -fsyntax-only %t/t.c -I %t/includes -isysroot %S/Inputs/SDK -fcas-path %t/cas -DSOME_MACRO -dependency-file %t/daemon.d -MT deps

// RUN: diff -u %t/inline.rsp %t/daemon.rsp
// RUN: diff -u %t/inline.d %t/daemon.d
// RUN: diff -u %t/inline.root %t/daemon.root
// RUN: FileCheck %s --check-prefix=ROOT --input-file %t/daemon.root
// ROOT: llvmcas://

//--- t.c
#include "t.h"

int test(struct S *s) {
  return s->x;
}

//--- includes/t.h
struct S {
  int x;
};

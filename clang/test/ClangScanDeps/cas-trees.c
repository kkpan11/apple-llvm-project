// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: sed -e "s|DIR|%/t|g" %t/cdb.json.template > %t/cdb.json

// RUN: clang-scan-deps -compilation-database %t/cdb.json -cas-path %t/cas -format experimental-tree -mode preprocess-minimized-sources > %t/result1.txt
// RUN: clang-scan-deps -compilation-database %t/cdb.json -cas-path %t/cas -format experimental-tree -mode preprocess > %t/result2.txt
// RUN: diff -u %t/result1.txt %t/result2.txt
// RUN: FileCheck %s -input-file %t/result1.txt -DPREFIX=%/t

// CHECK:      tree {{.*}} for '[[PREFIX]]/t1.c'
// CHECK-NEXT: tree {{.*}} for '[[PREFIX]]/t2.c'


//--- cdb.json.template
[
  {
    "directory": "DIR",
    "command": "clang -fsyntax-only DIR/t1.c",
    "file": "DIR/t1.c"
  },
  {
    "directory": "DIR",
    "command": "clang -fsyntax-only DIR/t2.c",
    "file": "DIR/t2.c"
  }
]

//--- t1.c
#include "top.h"
#include "n1.h"

//--- t2.c
#include "n1.h"

//--- top.h
#ifndef _TOP_H_
#define _TOP_H_

#define WHATEVER 1
#include "n1.h"

struct S {
  int x;
};

#endif

//--- n1.h
#ifndef _N1_H_
#define _N1_H_

int x1;

#endif

; RUN: rm -rf %t && mkdir -p %t
; RUN: llc %s --filetype=obj -o %t/object.o
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent > %t/default.id
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent -dead-strip > %t/dead-strip.id
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent -dead-strip \
; RUN:    -keep-alive-section="__DATA,X" > %t/keep-alive-X.id
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent -dead-strip \
; RUN:    -keep-alive-section="__DATA,X" -keep-alive-section="__DATA,Y" \
; RUN:    > %t/keep-alive-XY.id
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent -dead-strip-section="__DATA,X" > %t/dead-strip-X.id
; RUN: llvm-cas-object-format --cas %t/cas %t/object.o --ingest-schema=nestedv1 \
; RUN:    -silent -dead-strip-section="__DATA,X" -dead-strip-section="__DATA,Y" \
; RUN:    > %t/dead-strip-XY.id
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/default.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK,KEEP-BSS,KEEP-X,KEEP-Y
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/dead-strip.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/dead-strip-X.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK,KEEP-BSS,KEEP-Y
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/dead-strip-XY.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK,KEEP-BSS
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/keep-alive-X.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK,KEEP-X
; RUN: llvm-cas-object-format --cas %t/cas --print-cas-object @%t/keep-alive-XY.id \
; RUN:   --print-cas-object-sort-sections \
; RUN:   | FileCheck %s -check-prefixes=CHECK,KEEP-X,KEEP-Y

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx12.0.0"

; CHECK-LABEL: SECTION: __DATA,X
; KEEP-X-NOT:  SYMBOL:
; KEEP-X:      SYMBOL: _deadX |
; CHECK-NOT:   SYMBOL:
; CHECK:       SYMBOL: _liveX |
; CHECK-NOT:   SYMBOL:
@deadX = internal global i32 0, section "__DATA,X"
@liveX = internal global i32 0, section "__DATA,X"

; CHECK-LABEL: SECTION: __DATA,Y
; KEEP-Y-NOT:  SYMBOL:
; KEEP-Y:      SYMBOL: _deadY |
; CHECK-NOT:   SYMBOL:
; CHECK:       SYMBOL: _liveY |
; CHECK-NOT:   SYMBOL:
@deadY = internal global i32 0, section "__DATA,Y"
@liveY = internal global i32 0, section "__DATA,Y"

; CHECK-LABEL:  SECTION: __DATA,__bss
; KEEP-BSS-NOT: SYMBOL:
; KEEP-BSS:     SYMBOL: _dead |
; CHECK-NOT:    SYMBOL:
; CHECK:        SYMBOL: _live |
; CHECK-NOT:    SYMBOL:
@dead = internal global i32 0
@live = internal global i32 0

; CHECK-LABEL: SECTION: __DATA,__data
; CHECK-NOT:   SYMBOL:
; CHECK:       SYMBOL: _user |
; CHECK-NOT:   SYMBOL:
; CHECK:       SYMBOL: _userX |
; CHECK-NOT:   SYMBOL:
; CHECK:       SYMBOL: _userY |
; CHECK-NOT:   SYMBOL:
@user = global i32* @live
@userX = global i32* @liveX
@userY = global i32* @liveY

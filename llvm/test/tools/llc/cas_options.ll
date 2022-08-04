; REQUIRES: default_triple
; RUN: not llc -o /dev/null --filetype=asm --cas-backend --mtriple=arm64-apple-ios13.4.0 \
; RUN:   2>&1 | FileCheck %s --check-prefix=OUTPUT_TYPE
; RUN: not llc -o /dev/null --filetype=null --cas-backend --mtriple=arm64-apple-ios13.4.0 \
; RUN:   2>&1 | FileCheck %s --check-prefix=OUTPUT_TYPE
; OUTPUT_TYPE: CAS Backend requires .obj output

; RUN: not llc -o /dev/null --filetype=obj --cas-backend --mtriple=i386-pc-linux-gnu \
; RUN:   2>&1 | FileCheck %s --check-prefix=NEEDS_MACHO
; NEEDS_MACHO: CAS Backend requires MachO format

define void @f() {
  ret void
}

; RUN: llc -mtriple=i386-apple-darwin %s -o - | FileCheck %s

declare void @clobber()

declare swifttailcc void @swifttail_callee()
define swifttailcc void @swifttail() {
; CHECK-LABEL: swifttail:
; CHECK-NOT: %rbx
  call void @clobber()
  tail call swifttailcc void @swifttail_callee()
  ret void
}

; RUN: llc -O0 --filetype=obj --cas-backend --cas=%t.casdb --mccas-casid -o %t.casid %s
; RUN: llvm-cas-dump --cas=%t.casdb --casid-file %t.casid --hex-dump | FileCheck %s

;CHECK:mc:debug_str
;CHECK-NEXT: 0x41 0x42 0x43 0x44

; This test is created from a C program like:
; int foo() { return 10; }

target triple = "arm64-apple-macosx12.0.0"

define i32 @foo() !dbg !9 {
  ret i32 10, !dbg !14
}
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6, !7}
!0 = distinct !DICompileUnit( language: DW_LANG_C99, file: !1, producer: "ABCD", emissionKind: FullDebug, casFriendly: DebugAbbrev)
!1 = !DIFile(filename: "test.c",
directory: "some_dir")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"wchar_size", i32 4}
!5 = !{i32 7, !"PIC Level", i32 2}
!6 = !{i32 7, !"uwtable", i32 2}
!7 = !{i32 7, !"frame-pointer", i32 1}
!9 = distinct !DISubprogram(name: "foo",
type: !10,
unit: !0,
retainedNodes: !13)
!10 = !DISubroutineType(types: !11)
!11 = !{!12}
!12 = !DIBasicType(name: "int",
encoding: DW_ATE_signed)
!13 = !{}
!14 = !DILocation(line: 3, column: 3, scope: !9)

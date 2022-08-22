;RUN: llc -O0 --filetype=obj --cas-backend  --cas=%t/cas  --mccas-casid %s -o %t/test.id
;RUN: llvm-cas-dump --cas=%t/cas --casid-file %t/test.id --hex-dump | FileCheck %s

;CHECK: mc:debug_info_cu llvmcas://{{[0-9a-z]+}}
;CHECK-NEXT: 0x3c 0x00 0x00 0x00 0x04 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x08 0x01 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x0c 0x00 0x6a 0x00 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x93 0x00 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x00 0x00 0x00 0x00 0x04 0x00
;CHECK-NEXT: 0x00 0x00 0x02 0x00 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x00 0x04 0x00 0x00 0x00 0x01
;CHECK-NEXT: 0x6f 0xb8 0x00 0x00 0x00 0x01 0x01 0x00
;CHECK-NEXT: mc:debug_info_cu llvmcas://{{[0-9a-z]+}} 
;CHECK-NEXT: 0x56 0x00 0x00 0x00 0x04 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x08 0x01 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x0c 0x00 0x6a 0x00 0x00 0x00 0x3c 0x00
;CHECK-NEXT: 0x00 0x00 0x93 0x00 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x00 0x00 0x00 0x00 0x18 0x00
;CHECK-NEXT: 0x00 0x00 0x02 0x00 0x00 0x00 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x00 0x18 0x00 0x00 0x00 0x01
;CHECK-NEXT: 0x6f 0xbc 0x00 0x00 0x00 0x01 0x02 0x52
;CHECK-NEXT: 0x00 0x00 0x00 0x03 0x02 0x91 0x0c 0xc4
;CHECK-NEXT: 0x00 0x00 0x00 0x01 0x02 0x52 0x00 0x00
;CHECK-NEXT: 0x00 0x00 0x04 0xc0 0x00 0x00 0x00 0x05
;CHECK-NEXT: 0x04 0x00

define void @foo() #0 !dbg !9 {
  ret void, !dbg !14
}
define i32 @bar(i32 noundef %x) #0 !dbg !15 {
entry:
  %x.addr = alloca i32, align 4
  store i32 %x, ptr %x.addr, align 4
  call void @llvm.dbg.declare(metadata ptr %x.addr, metadata !20, metadata !DIExpression()), !dbg !21
  %0 = load i32, ptr %x.addr, align 4, !dbg !22
  %add = add nsw i32 %0, 1, !dbg !23
  ret i32 %add, !dbg !24
}
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !7}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang version 16.0.0 (https://github.com/apple/llvm-project.git 24ab32497e30b8510454973371365693ccdbc718)",
 emissionKind: FullDebug, casFriendly: DebugAbbrev)
!1 = !DIFile(filename: "/Users/shubham/Development/testclang/a.c", directory: "/Users/shubham/Development/testclang")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!5 = !{i32 8, i32 2}
!7 = !{i32 7, !"frame-pointer", i32 1}
!9 = distinct !DISubprogram(name: "foo", file: !10, line: 1, type: !11, unit: !0, retainedNodes: !13)
!10 = !DIFile(filename: "foo.h", directory: "/Users/shubham/Development/testclang")
!11 = !DISubroutineType(types: !12)
!12 = !{null}
!13 = !{}
!14 = !DILocation(line: 2, column: 3, scope: !9)
!15 = distinct !DISubprogram(name: "bar", file: !16, line: 2, type: !17, unit: !0, retainedNodes: !13)
!16 = !DIFile(filename: "a.c", directory: "/Users/shubham/Development/testclang")
!17 = !DISubroutineType(types: !18)
!18 = !{!19, !19}
!19 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!20 = !DILocalVariable(name: "x", scope: !15, file: !16, line: 2, type: !19)
!21 = !DILocation(line: 2, scope: !15)
!22 = !DILocation(line: 3, scope: !15)
!23 = !DILocation(line: 3, scope: !15)
!24 = !DILocation(line: 3, scope: !15)

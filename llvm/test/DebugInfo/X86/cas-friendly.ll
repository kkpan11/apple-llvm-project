; RUN: llc -mtriple=x86_64-apple-macosx12.0.0 -filetype=obj %s -o %t 
; RUN: llvm-dwarfdump -debug-line -v %t | FileCheck %s

; CHECK: debug_line[0x00000000]
; CHECK-NEXT: Line table prologue:
; CHECK-NEXT:     total_length: 0x00000042
; CHECK-NEXT:           format: DWARF32
; CHECK-NEXT:          version: 4
; CHECK-NEXT:  prologue_length: 0x00000020
; CHECK-NEXT:  min_inst_length: 1
; CHECK-NEXT: max_ops_per_inst: 1
; CHECK-NEXT:  default_is_stmt: 1
; CHECK-NEXT:        line_base: -5
; CHECK-NEXT:       line_range: 14
; CHECK-NEXT:      opcode_base: 13
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_copy] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_line] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_file] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_column] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_negate_stmt] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_basic_block] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_const_add_pc] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_fixed_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_prologue_end] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_epilogue_begin] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_isa] = 1
; CHECK-NEXT: include_directories[  1] = "."
; CHECK-NEXT: file_names[  1]:
; CHECK-NEXT:            name: "test.c"
; CHECK-NEXT:       dir_index: 1
; CHECK-NEXT:        mod_time: 0x00000000
; CHECK-NEXT:          length: 0x00000000
; CHECK:             Address            Line   Column File   ISA Discriminator Flags
; CHECK-NEXT:             ------------------ ------ ------ ------ --- ------------- -------------
; CHECK-NEXT: 0x0000002a: 00 DW_LNE_set_address (0x0000000000000000)
; CHECK-NEXT: 0x00000035: 01 DW_LNS_copy
; CHECK-NEXT:             0x0000000000000000      1      0      1   0             0  is_stmt
; CHECK-NEXT: 0x00000036: 05 DW_LNS_set_column (9)
; CHECK-NEXT: 0x00000038: 0a DW_LNS_set_prologue_end
; CHECK-NEXT: 0x00000039: 75 address += 7,  line += 1
; CHECK-NEXT:             0x0000000000000007      2      9      1   0             0  is_stmt prologue_end
; CHECK-NEXT: 0x0000003a: 05 DW_LNS_set_column (10)
; CHECK-NEXT: 0x0000003c: 06 DW_LNS_negate_stmt
; CHECK-NEXT: 0x0000003d: 3c address += 3,  line += 0
; CHECK-NEXT:             0x000000000000000a      2     10      1   0             0 
; CHECK-NEXT: 0x0000003e: 05 DW_LNS_set_column (2)
; CHECK-NEXT: 0x00000040: 4a address += 4,  line += 0
; CHECK-NEXT:             0x000000000000000e      2      2      1   0             0 
; CHECK-NEXT: 0x00000041: 02 DW_LNS_advance_pc (2)
; CHECK-NEXT: 0x00000043: 00 DW_LNE_end_sequence
; CHECK-NEXT:             0x0000000000000010      2      2      1   0             0  end_sequence

; CHECK: debug_line[0x00000046]
; CHECK-NEXT: Line table prologue:
; CHECK-NEXT:     total_length: 0x00000042
; CHECK-NEXT:           format: DWARF32
; CHECK-NEXT:          version: 4
; CHECK-NEXT:  prologue_length: 0x00000020
; CHECK-NEXT:  min_inst_length: 1
; CHECK-NEXT: max_ops_per_inst: 1
; CHECK-NEXT:  default_is_stmt: 1
; CHECK-NEXT:        line_base: -5
; CHECK-NEXT:       line_range: 14
; CHECK-NEXT:      opcode_base: 13
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_copy] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_line] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_file] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_column] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_negate_stmt] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_basic_block] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_const_add_pc] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_fixed_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_prologue_end] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_epilogue_begin] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_isa] = 1
; CHECK-NEXT: include_directories[  1] = "."
; CHECK-NEXT: file_names[  1]:
; CHECK-NEXT:            name: "test.c"
; CHECK-NEXT:       dir_index: 1
; CHECK-NEXT:        mod_time: 0x00000000
; CHECK-NEXT:          length: 0x00000000
; CHECK:             Address            Line   Column File   ISA Discriminator Flags
; CHECK-NEXT:             ------------------ ------ ------ ------ --- ------------- -------------
; CHECK-NEXT: 0x00000070: 00 DW_LNE_set_address (0x0000000000000010)
; CHECK-NEXT: 0x0000007b: 16 address += 0,  line += 4
; CHECK-NEXT:             0x0000000000000010      5      0      1   0             0  is_stmt
; CHECK-NEXT: 0x0000007c: 05 DW_LNS_set_column (11)
; CHECK-NEXT: 0x0000007e: 0a DW_LNS_set_prologue_end
; CHECK-NEXT: 0x0000007f: 75 address += 7,  line += 1
; CHECK-NEXT:             0x0000000000000017      6     11      1   0             0  is_stmt prologue_end
; CHECK-NEXT: 0x00000080: 05 DW_LNS_set_column (10)
; CHECK-NEXT: 0x00000082: 06 DW_LNS_negate_stmt
; CHECK-NEXT: 0x00000083: 3c address += 3,  line += 0
; CHECK-NEXT:             0x000000000000001a      6     10      1   0             0 
; CHECK-NEXT: 0x00000084: 05 DW_LNS_set_column (2)
; CHECK-NEXT: 0x00000086: 3c address += 3,  line += 0
; CHECK-NEXT:             0x000000000000001d      6      2      1   0             0 
; CHECK-NEXT: 0x00000087: 02 DW_LNS_advance_pc (2)
; CHECK-NEXT: 0x00000089: 00 DW_LNE_end_sequence
; CHECK-NEXT:             0x000000000000001f      6      2      1   0             0  end_sequence

; CHECK: debug_line[0x0000008c]
; CHECK-NEXT: Line table prologue:
; CHECK-NEXT:     total_length: 0x0000004c
; CHECK-NEXT:           format: DWARF32
; CHECK-NEXT:          version: 4
; CHECK-NEXT:  prologue_length: 0x00000020
; CHECK-NEXT:  min_inst_length: 1
; CHECK-NEXT: max_ops_per_inst: 1
; CHECK-NEXT:  default_is_stmt: 1
; CHECK-NEXT:        line_base: -5
; CHECK-NEXT:       line_range: 14
; CHECK-NEXT:      opcode_base: 13
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_copy] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_advance_line] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_file] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_column] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_negate_stmt] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_basic_block] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_const_add_pc] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_fixed_advance_pc] = 1
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_prologue_end] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_epilogue_begin] = 0
; CHECK-NEXT: standard_opcode_lengths[DW_LNS_set_isa] = 1
; CHECK-NEXT: include_directories[  1] = "."
; CHECK-NEXT: file_names[  1]:
; CHECK-NEXT:            name: "test.c"
; CHECK-NEXT:       dir_index: 1
; CHECK-NEXT:        mod_time: 0x00000000
; CHECK-NEXT:          length: 0x00000000
; CHECK:             Address            Line   Column File   ISA Discriminator Flags
; CHECK-NEXT:             ------------------ ------ ------ ------ --- ------------- -------------
; CHECK-NEXT: 0x000000b6: 00 DW_LNE_set_address (0x0000000000000020)
; CHECK-NEXT: 0x000000c1: 1a address += 0,  line += 8
; CHECK-NEXT:             0x0000000000000020      9      0      1   0             0  is_stmt
; CHECK-NEXT: 0x000000c2: 05 DW_LNS_set_column (13)
; CHECK-NEXT: 0x000000c4: 0a DW_LNS_set_prologue_end
; CHECK-NEXT: 0x000000c5: 08 DW_LNS_const_add_pc (0x0000000000000011)
; CHECK-NEXT: 0x000000c6: 67 address += 6,  line += 1
; CHECK-NEXT:             0x0000000000000037     10     13      1   0             0  is_stmt prologue_end
; CHECK-NEXT: 0x000000c7: 05 DW_LNS_set_column (9)
; CHECK-NEXT: 0x000000c9: 06 DW_LNS_negate_stmt
; CHECK-NEXT: 0x000000ca: 3c address += 3,  line += 0
; CHECK-NEXT:             0x000000000000003a     10      9      1   0             0 
; CHECK-NEXT: 0x000000cb: 05 DW_LNS_set_column (25)
; CHECK-NEXT: 0x000000cd: 74 address += 7,  line += 0
; CHECK-NEXT:             0x0000000000000041     10     25      1   0             0 
; CHECK-NEXT: 0x000000ce: 05 DW_LNS_set_column (21)
; CHECK-NEXT: 0x000000d0: 3c address += 3,  line += 0
; CHECK-NEXT:             0x0000000000000044     10     21      1   0             0 
; CHECK-NEXT: 0x000000d1: 05 DW_LNS_set_column (19)
; CHECK-NEXT: 0x000000d3: 58 address += 5,  line += 0
; CHECK-NEXT:             0x0000000000000049     10     19      1   0             0 
; CHECK-NEXT: 0x000000d4: 05 DW_LNS_set_column (2)
; CHECK-NEXT: 0x000000d6: 2e address += 2,  line += 0
; CHECK-NEXT:             0x000000000000004b     10      2      1   0             0 
; CHECK-NEXT: 0x000000d7: 02 DW_LNS_advance_pc (9)
; CHECK-NEXT: 0x000000d9: 00 DW_LNE_end_sequence
; CHECK-NEXT:             0x0000000000000054     10      2      1   0             0  end_sequence

; ModuleID = './test.c'
source_filename = "./test.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx12.0.0"

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @foo(i32 noundef %n) #0 !dbg !9 {
entry:
  %n.addr = alloca i32, align 4
  store i32 %n, i32* %n.addr, align 4
  call void @llvm.dbg.declare(metadata i32* %n.addr, metadata !15, metadata !DIExpression()), !dbg !16
  %0 = load i32, i32* %n.addr, align 4, !dbg !17
  %1 = load i32, i32* %n.addr, align 4, !dbg !18
  %mul = mul nsw i32 %0, %1, !dbg !19
  ret i32 %mul, !dbg !20
}

; Function Attrs: nofree nosync nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @bar(i32 noundef %n) #0 !dbg !21 {
entry:
  %n.addr = alloca i32, align 4
  store i32 %n, i32* %n.addr, align 4
  call void @llvm.dbg.declare(metadata i32* %n.addr, metadata !22, metadata !DIExpression()), !dbg !23
  %0 = load i32, i32* %n.addr, align 4, !dbg !24
  %mul = mul nsw i32 2, %0, !dbg !25
  ret i32 %mul, !dbg !26
}

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @main(i32 noundef %argc, i8** noundef %argv) #0 !dbg !27 {
entry:
  %retval = alloca i32, align 4
  %argc.addr = alloca i32, align 4
  %argv.addr = alloca i8**, align 8
  store i32 0, i32* %retval, align 4
  store i32 %argc, i32* %argc.addr, align 4
  call void @llvm.dbg.declare(metadata i32* %argc.addr, metadata !33, metadata !DIExpression()), !dbg !34
  store i8** %argv, i8*** %argv.addr, align 8
  call void @llvm.dbg.declare(metadata i8*** %argv.addr, metadata !35, metadata !DIExpression()), !dbg !36
  %0 = load i32, i32* %argc.addr, align 4, !dbg !37
  %call = call i32 @foo(i32 noundef %0), !dbg !38
  %1 = load i32, i32* %argc.addr, align 4, !dbg !39
  %call1 = call i32 @bar(i32 noundef %1), !dbg !40
  %add = add nsw i32 %call, %call1, !dbg !41
  ret i32 %add, !dbg !42
}

attributes #0 = { noinline nounwind optnone ssp uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree nosync nounwind readnone speculatable willreturn }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang version 14.0.0 (git@github.com:apple/llvm-project.git 24e049152a8c2239a157a36f6a4d065e7a1b183b)", isOptimized: false, runtimeVersion: 0, emissionKind: CasFriendly, splitDebugInlining: false, nameTableKind: None, sysroot: "/")
!1 = !DIFile(filename: "test.c", directory: "/Users/shubham/Development/testClangDebugInfo")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"wchar_size", i32 4}
!5 = !{i32 7, !"PIC Level", i32 2}
!6 = !{i32 7, !"uwtable", i32 1}
!7 = !{i32 7, !"frame-pointer", i32 2}
!8 = !{!"clang version 14.0.0 (git@github.com:apple/llvm-project.git)"}
!9 = distinct !DISubprogram(name: "foo", scope: !10, file: !10, line: 1, type: !11, scopeLine: 1, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !14)
!10 = !DIFile(filename: "./test.c", directory: "/Users/shubham/Development/testClangDebugInfo")
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{}
!15 = !DILocalVariable(name: "n", arg: 1, scope: !9, file: !10, line: 1, type: !13)
!16 = !DILocation(line: 1, column: 13, scope: !9)
!17 = !DILocation(line: 2, column: 9, scope: !9)
!18 = !DILocation(line: 2, column: 11, scope: !9)
!19 = !DILocation(line: 2, column: 10, scope: !9)
!20 = !DILocation(line: 2, column: 2, scope: !9)
!21 = distinct !DISubprogram(name: "bar", scope: !10, file: !10, line: 5, type: !11, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !14)
!22 = !DILocalVariable(name: "n", arg: 1, scope: !21, file: !10, line: 5, type: !13)
!23 = !DILocation(line: 5, column: 13, scope: !21)
!24 = !DILocation(line: 6, column: 11, scope: !21)
!25 = !DILocation(line: 6, column: 10, scope: !21)
!26 = !DILocation(line: 6, column: 2, scope: !21)
!27 = distinct !DISubprogram(name: "main", scope: !10, file: !10, line: 9, type: !28, scopeLine: 9, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !14)
!28 = !DISubroutineType(types: !29)
!29 = !{!13, !13, !30}
!30 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !31, size: 64)
!31 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !32, size: 64)
!32 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!33 = !DILocalVariable(name: "argc", arg: 1, scope: !27, file: !10, line: 9, type: !13)
!34 = !DILocation(line: 9, column: 14, scope: !27)
!35 = !DILocalVariable(name: "argv", arg: 2, scope: !27, file: !10, line: 9, type: !30)
!36 = !DILocation(line: 9, column: 27, scope: !27)
!37 = !DILocation(line: 10, column: 13, scope: !27)
!38 = !DILocation(line: 10, column: 9, scope: !27)
!39 = !DILocation(line: 10, column: 25, scope: !27)
!40 = !DILocation(line: 10, column: 21, scope: !27)
!41 = !DILocation(line: 10, column: 19, scope: !27)
!42 = !DILocation(line: 10, column: 2, scope: !27)

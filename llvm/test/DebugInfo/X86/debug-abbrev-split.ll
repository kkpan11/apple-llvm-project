; RUN: llc -mtriple=arm64-apple-darwin21.5.0 -filetype=obj %s -o %t
; RUN: llvm-dwarfdump --debug-info --debug-abbrev %t | FileCheck %s


; CHECK: .debug_abbrev contents:
; CHECK-NEXT: Abbrev table for offset: 0x0000[[ABBREV_OFFSET1:[0-9a-z]+]]
; CHECK-NEXT: [1] DW_TAG_compile_unit DW_CHILDREN_yes

; CHECK: Abbrev table for offset: 0x0000[[ABBREV_OFFSET2:[0-9a-z]+]]
; CHECK-NEXT: [1] DW_TAG_compile_unit DW_CHILDREN_yes

; CHECK: Abbrev table for offset: 0x0000[[ABBREV_OFFSET3:[0-9a-z]+]]
; CHECK-NEXT: [1] DW_TAG_compile_unit DW_CHILDREN_yes

; CHECK: .debug_info contents:
; CHECK-NEXT: {{[a-z0-9]+}}: Compile Unit: length = {{[a-z0-9]+}}, format = DWARF32, version = {{[a-z0-9]+}}, abbr_offset = 0x[[ABBREV_OFFSET1]], addr_size = {{[a-z0-9]+}}{{.*}}

; CHECK: {{[a-z0-9]+}}: Compile Unit: length = {{[a-z0-9]+}}, format = DWARF32, version = {{[a-z0-9]+}}, abbr_offset = 0x[[ABBREV_OFFSET2]], addr_size = {{[a-z0-9]+}}{{.*}}

; CHECK: {{[a-z0-9]+}}: Compile Unit: length = {{[a-z0-9]+}}, format = DWARF32, version = {{[a-z0-9]+}}, abbr_offset = 0x[[ABBREV_OFFSET3]], addr_size = {{[a-z0-9]+}}{{.*}}

; ModuleID = 'a.cpp'
source_filename = "a.cpp"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx12.0.0"

; Function Attrs: mustprogress noinline optnone ssp uwtable
define noundef i32 @_Z1av() #0 !dbg !13 {
entry:
  %call = call noundef i32 @_Z3fooIiET_S0_(i32 noundef 2), !dbg !18
  ret i32 %call, !dbg !19
}

; Function Attrs: mustprogress noinline nounwind optnone ssp uwtable
define linkonce_odr noundef i32 @_Z3fooIiET_S0_(i32 noundef %s) #1 !dbg !20 {
entry:
  %s.addr = alloca i32, align 4
  store i32 %s, ptr %s.addr, align 4
  call void @llvm.dbg.declare(metadata ptr %s.addr, metadata !26, metadata !DIExpression()), !dbg !27
  %0 = load i32, ptr %s.addr, align 4, !dbg !28
  %1 = load i32, ptr %s.addr, align 4, !dbg !29
  %mul = mul nsw i32 %0, %1, !dbg !30
  ret i32 %mul, !dbg !31
}

; Function Attrs: mustprogress noinline nounwind optnone ssp uwtable
define noundef i32 @_Z1bi(i32 noundef %x) #1 !dbg !32 {
entry:
  %x.addr = alloca i32, align 4
  store i32 %x, ptr %x.addr, align 4
  call void @llvm.dbg.declare(metadata ptr %x.addr, metadata !33, metadata !DIExpression()), !dbg !34
  %0 = load i32, ptr %x.addr, align 4, !dbg !35
  %1 = load i32, ptr %x.addr, align 4, !dbg !36
  %mul = mul nsw i32 %0, %1, !dbg !37
  ret i32 %mul, !dbg !38
}

; Function Attrs: nocallback nofree nosync nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #2

attributes #0 = { mustprogress noinline optnone ssp uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+crc,+crypto,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+sha3,+sm4,+v8.5a,+zcm,+zcz" }
attributes #1 = { mustprogress noinline nounwind optnone ssp uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+crc,+crypto,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+sha3,+sm4,+v8.5a,+zcm,+zcz" }
attributes #2 = { nocallback nofree nosync nounwind readnone speculatable willreturn }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6, !7, !8, !9, !10, !11}
!llvm.ident = !{!12}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "clang version 15.0.0 (git@github.com:apple/llvm-project.git a9613e5bc4afee75335dfc5fbb92954264c04e85)", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None, sysroot: "/", casFriendly: DebugAbbrev)
!1 = !DIFile(filename: "a.cpp", directory: "/Users/shubhamrastogi/Development/testCASDeDupe")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"wchar_size", i32 4}
!5 = !{i32 8, !"branch-target-enforcement", i32 0}
!6 = !{i32 8, !"sign-return-address", i32 0}
!7 = !{i32 8, !"sign-return-address-all", i32 0}
!8 = !{i32 8, !"sign-return-address-with-bkey", i32 0}
!9 = !{i32 7, !"PIC Level", i32 2}
!10 = !{i32 7, !"uwtable", i32 2}
!11 = !{i32 7, !"frame-pointer", i32 1}
!12 = !{!"clang version 15.0.0 (git@github.com:apple/llvm-project.git)"}
!13 = distinct !DISubprogram(name: "a", linkageName: "_Z1av", scope: !1, file: !1, line: 2, type: !14, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !17)
!14 = !DISubroutineType(types: !15)
!15 = !{!16}
!16 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!17 = !{}
!18 = !DILocation(line: 3, column: 9, scope: !13)
!19 = !DILocation(line: 3, column: 2, scope: !13)
!20 = distinct !DISubprogram(name: "foo<int>", linkageName: "_Z3fooIiET_S0_", scope: !21, file: !21, line: 2, type: !22, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, templateParams: !24, retainedNodes: !17)
!21 = !DIFile(filename: "./foo.h", directory: "/Users/shubhamrastogi/Development/testCASDeDupe")
!22 = !DISubroutineType(types: !23)
!23 = !{!16, !16}
!24 = !{!25}
!25 = !DITemplateTypeParameter(name: "T", type: !16)
!26 = !DILocalVariable(name: "s", arg: 1, scope: !20, file: !21, line: 2, type: !16)
!27 = !DILocation(line: 2, column: 9, scope: !20)
!28 = !DILocation(line: 3, column: 12, scope: !20)
!29 = !DILocation(line: 3, column: 14, scope: !20)
!30 = !DILocation(line: 3, column: 13, scope: !20)
!31 = !DILocation(line: 3, column: 5, scope: !20)
!32 = distinct !DISubprogram(name: "b", linkageName: "_Z1bi", scope: !1, file: !1, line: 6, type: !22, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !17)
!33 = !DILocalVariable(name: "x", arg: 1, scope: !32, file: !1, line: 6, type: !16)
!34 = !DILocation(line: 6, column: 11, scope: !32)
!35 = !DILocation(line: 7, column: 9, scope: !32)
!36 = !DILocation(line: 7, column: 11, scope: !32)
!37 = !DILocation(line: 7, column: 10, scope: !32)
!38 = !DILocation(line: 7, column: 2, scope: !32)

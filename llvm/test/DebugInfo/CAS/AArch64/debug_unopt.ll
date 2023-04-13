; RUN: llc -debug-info-unopt -O0 --filetype=obj --cas-backend --cas=%t/cas --mccas-casid %s -o %t/debug_unopt.id
; RUN: llvm-cas-dump --cas=%t/cas --casid-file %t/debug_unopt.id | FileCheck %s

; CHECK: mc:debug_string_section llvmcas://
; CHECK-NEXT: mc:debug_string llvmcas://
; CHECK-NEXT: mc:section      llvmcas://

; CHECK: mc:debug_line_section llvmcas://
; CHECK-NEXT: mc:debug_line_unopt llvmcas://
; CHECK-NEXT: mc:padding      llvmcas://

; ModuleID = 'a.cpp'
source_filename = "a.cpp"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx13.0.0"

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define i32 @_Z3fooj(i32 noundef %0) #0 !dbg !10 {
  %2 = alloca i32, align 4
  store i32 %0, ptr %2, align 4
  call void @llvm.dbg.declare(metadata ptr %2, metadata !16, metadata !DIExpression()), !dbg !17
  ret i32 1, !dbg !18
}

; Function Attrs: nocallback nofree nosync nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define i32 @_Z3bari(i32 noundef %0) #0 !dbg !19 {
  %2 = alloca i32, align 4
  store i32 %0, ptr %2, align 4
  call void @llvm.dbg.declare(metadata ptr %2, metadata !23, metadata !DIExpression()), !dbg !24
  %3 = load i32, ptr %2, align 4, !dbg !25
  %4 = load i32, ptr %2, align 4, !dbg !26
  %5 = call i32 @_Z3fooj(i32 noundef %4) #2, !dbg !27
  %6 = add i32 %3, %5, !dbg !28
  ret i32 %6, !dbg !29
}

attributes #0 = { noinline nounwind optnone ssp uwtable(sync) "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-builtin-calloc" "no-builtin-stpcpy" "no-trapping-math"="true" "probe-stack"="__chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+amx,+amx2,+crc,+crypto,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+sha3,+sm4,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8.5a,+v8a,+zcm,+zcz" }
attributes #1 = { nocallback nofree nosync nounwind readnone speculatable willreturn }
attributes #2 = { "no-builtin-calloc" "no-builtin-stpcpy" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}
!llvm.dbg.cu = !{!7}
!llvm.ident = !{!9}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 14, i32 0]}
!1 = !{i32 7, !"Dwarf Version", i32 4}
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"uwtable", i32 1}
!6 = !{i32 7, !"frame-pointer", i32 1}
!7 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !8, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None, sysroot: "/Users/shubham")
!8 = !DIFile(filename: "a.cpp", directory: "/Users/shubham/Development/")
!9 = !{!"clang]"}
!10 = distinct !DISubprogram(name: "foo", linkageName: "_Z3fooj", scope: !11, file: !11, line: 1, type: !12, scopeLine: 1, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !15)
!11 = !DIFile(filename: "./foo.h", directory: "/Users/shubham/Development/")
!12 = !DISubroutineType(types: !13)
!13 = !{!14, !14}
!14 = !DIBasicType(name: "unsigned int", size: 32, encoding: DW_ATE_unsigned)
!15 = !{}
!16 = !DILocalVariable(name: "y", arg: 1, scope: !10, file: !11, line: 1, type: !14)
!17 = !DILocation(line: 1, column: 23, scope: !10)
!18 = !DILocation(line: 2, column: 3, scope: !10)
!19 = distinct !DISubprogram(name: "bar", linkageName: "_Z3bari", scope: !8, file: !8, line: 2, type: !20, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !15)
!20 = !DISubroutineType(types: !21)
!21 = !{!22, !22}
!22 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!23 = !DILocalVariable(name: "x", arg: 1, scope: !19, file: !8, line: 2, type: !22)
!24 = !DILocation(line: 2, column: 13, scope: !19)
!25 = !DILocation(line: 3, column: 10, scope: !19)
!26 = !DILocation(line: 3, column: 16, scope: !19)
!27 = !DILocation(line: 3, column: 12, scope: !19)
!28 = !DILocation(line: 3, column: 11, scope: !19)
!29 = !DILocation(line: 3, column: 3, scope: !19)

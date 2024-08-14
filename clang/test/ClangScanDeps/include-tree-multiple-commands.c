// Test scanning when the driver requires multiple jobs. E.g. with -save-temps
// there will be separate -E, -emit-llvm-bc, -S, and -cc1as jobs, which should
// each result in a "command" in the output.

// We use an x86_64-apple-darwin target to avoid host-dependent behaviour in
// the driver. Platforms without an integrated assembler have different commands
// REQUIRES: x86-registered-target

// RUN: rm -rf %t
// RUN: split-file %s %t

// RUN: mv %t/tu_define_foo_0.c %t/tu.c
// RUN: clang-scan-deps -format experimental-include-tree-full -cas-path %t/cas -module-files-dir %t/modules \
// RUN:   -- %clang -target x86_64-apple-darwin -c %t/tu.c -save-temps=obj -o %t/tu.o \
// RUN:     -fmodules -fimplicit-modules -fimplicit-module-maps -fmodules-cache-path=%t/cache \
// RUN:   > %t/deps.0.json

// RUN: cat %t/deps.0.json | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t

// RUN: %deps-to-rsp %t/deps.0.json --module-name=Mod             > %t/Mod.0.rsp
// RUN: %deps-to-rsp %t/deps.0.json --tu-index 0 --tu-cmd-index 0 > %t/tu-cpp.0.rsp
// RUN: %deps-to-rsp %t/deps.0.json --tu-index 0 --tu-cmd-index 1 > %t/tu-emit-ir.0.rsp
// RUN: %deps-to-rsp %t/deps.0.json --tu-index 0 --tu-cmd-index 2 > %t/tu-emit-asm.0.rsp
// RUN: %deps-to-rsp %t/deps.0.json --tu-index 0 --tu-cmd-index 3 > %t/tu-cc1as.0.rsp
// RUN: %clang @%t/Mod.0.rsp         -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: %clang @%t/tu-cpp.0.rsp      -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: %clang @%t/tu-emit-ir.0.rsp  -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: %clang @%t/tu-emit-asm.0.rsp -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: %clang @%t/tu-cc1as.0.rsp
// RUN: mv %t/tu.i  %t/tu.0.i
// RUN: mv %t/tu.bc %t/tu.0.bc
// RUN: mv %t/tu.s  %t/tu.0.s
// RUN: mv %t/tu.o  %t/tu.0.o

// RUN: mv %t/tu_define_foo_1.c %t/tu.c
// RUN: clang-scan-deps -format experimental-include-tree-full -cas-path %t/cas -module-files-dir %t/modules \
// RUN:   -- %clang -target x86_64-apple-darwin -c %t/tu.c -save-temps=obj -o %t/tu.o \
// RUN:     -fmodules -fimplicit-modules -fimplicit-module-maps -fmodules-cache-path=%t/cache \
// RUN:   > %t/deps.1.json

// The dependency graph has identical structure, just the include-tree ID and dependent cache keys are different.
// RUN: cat %t/deps.1.json | sed 's:\\\\\?:/:g' | FileCheck %s -DPREFIX=%/t
// RUN: not diff %t/deps.1.json %t/deps.0.json

// RUN: %deps-to-rsp %t/deps.1.json --module-name=Mod             > %t/Mod.1.rsp
// RUN: %deps-to-rsp %t/deps.1.json --tu-index 0 --tu-cmd-index 0 > %t/tu-cpp.1.rsp
// RUN: %deps-to-rsp %t/deps.1.json --tu-index 0 --tu-cmd-index 1 > %t/tu-emit-ir.1.rsp
// RUN: %deps-to-rsp %t/deps.1.json --tu-index 0 --tu-cmd-index 2 > %t/tu-emit-asm.1.rsp
// RUN: %deps-to-rsp %t/deps.1.json --tu-index 0 --tu-cmd-index 3 > %t/tu-cc1as.1.rsp
// RUN: %clang @%t/Mod.1.rsp         -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-HIT
// RUN: %clang @%t/tu-cpp.1.rsp      -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: %clang @%t/tu-emit-ir.1.rsp  -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-HIT
// RUN: %clang @%t/tu-emit-asm.1.rsp -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-HIT
// RUN: %clang @%t/tu-cc1as.1.rsp
// RUN: mv %t/tu.i  %t/tu.1.i
// RUN: mv %t/tu.bc %t/tu.1.bc
// RUN: mv %t/tu.s  %t/tu.1.s
// RUN: mv %t/tu.o  %t/tu.1.o

// RUN: diff %t/tu.1.i  %t/tu.0.i
// RUN: diff %t/tu.1.bc %t/tu.0.bc
// RUN: diff %t/tu.1.s  %t/tu.0.s
// RUN: diff %t/tu.1.o  %t/tu.0.o

// CACHE-HIT: remark: compile job cache hit
// CACHE-MISS: remark: compile job cache miss

// CHECK:      "modules": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "cache-key": "[[M_CACHE_KEY:llvmcas://[[:xdigit:]]+]]"
// CHECK-NEXT:     "cas-include-tree-id": "[[M_INCLUDE_TREE:llvmcas://[[:xdigit:]]+]]"
// CHECK-NEXT:     "clang-module-deps": []
// CHECK-NEXT:     "clang-modulemap-file": "[[PREFIX]]/module.modulemap"
// CHECK-NEXT:     "command-line": [
// CHECK:            "-fcas-include-tree"
// CHECK-NEXT:       "[[M_INCLUDE_TREE]]"
// CHECK:          ]
// CHECK:          "name": "Mod"
// CHECK-NEXT:   }
// CHECK-NEXT: ]
// CHECK-NEXT: "translation-units": [
// CHECK-NEXT:   {
// CHECK:          "commands": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "cache-key": "[[CPP_CACHE_KEY:llvmcas://[[:xdigit:]]+]]"
// CHECK-NEXT:         "cas-include-tree-id": "[[CPP_INCLUDE_TREE:llvmcas://[[:xdigit:]]+]]"
// CHECK-NEXT:         "clang-context-hash": "{{.*}}"
// CHECK-NEXT:         "clang-module-deps": [
// CHECK-NEXT:           {
// CHECK-NEXT:             "context-hash": "{{.*}}
// CHECK-NEXT:             "module-name": "Mod"
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:         "command-line": [
// CHECK-NEXT:           "-cc1"
// CHECK:                "-o"
// CHECK-NEXT:           "[[PREFIX]]/tu.i"
// CHECK:                "-fcas-include-tree"
// CHECK-NEXT:           "[[CPP_INCLUDE_TREE]]"
// CHECK-NOT:            "-fcas-input-file-cache-key"
// CHECK:                "-E"
// CHECK:                "-fmodule-file-cache-key"
// CHECK-NEXT:           "[[PREFIX]]/modules/{{.*}}/Mod-{{.*}}.pcm"
// CHECK-NEXT:           "[[M_CACHE_KEY]]"
// CHECK:                "-x"
// CHECK-NEXT:           "c"
// CHECK-NOT:            "{{.*}}tu.c"
// CHECK:                "-fmodule-file={{.*}}[[PREFIX]]/modules/{{.*}}/Mod-{{.*}}.pcm"
// CHECK:              ]
// CHECK:              "file-deps": [
// CHECK-NEXT:           "[[PREFIX]]/tu.c"
// CHECK-NEXT:         ]
// CHECK:              "input-file": "[[PREFIX]]/tu.c"
// CHECK-NEXT:       }
// CHECK-NEXT:       {
// CHECK-NEXT:         "cache-key": "[[COMPILER_CACHE_KEY:llvmcas://[[:xdigit:]]+]]"
// FIXME: This should be empty.
// CHECK-NEXT:         "cas-include-tree-id": "{{.*}}"
// CHECK-NEXT:         "clang-context-hash": "{{.*}}"
// CHECK-NEXT:         "clang-module-deps": [
// CHECK-NEXT:           {
// CHECK-NEXT:             "context-hash": "{{.*}}
// CHECK-NEXT:             "module-name": "Mod"
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:         "command-line": [
// CHECK-NEXT:           "-cc1"
// CHECK:                "-o"
// CHECK-NEXT:           "[[PREFIX]]/tu.bc"
// CHECK-NOT:            "-fcas-include-tree"
// CHECK:                "-fcas-input-file-cache-key"
// CHECK-NEXT:           "[[CPP_CACHE_KEY]]"
// CHECK:                "-emit-llvm-bc"
// CHECK:                "-fmodule-file-cache-key"
// CHECK-NEXT:           "[[PREFIX]]/modules/{{.*}}/Mod-{{.*}}.pcm"
// CHECK-NEXT:           "[[M_CACHE_KEY]]"
// CHECK:                "-x"
// CHECK-NEXT:           "c-cpp-output"
// CHECK-NOT:            "{{.*}}tu.i"
// CHECK:                "-fmodule-file={{.*}}[[PREFIX]]/modules/{{.*}}/Mod-{{.*}}.pcm"
// CHECK:              ]
// CHECK:              "input-file": "[[PREFIX]]{{.}}tu.c"
// CHECK-NEXT:       }
// CHECK-NEXT:       {
// CHECK-NEXT:         "cache-key": "[[BACKEND_CACHE_KEY:llvmcas://[[:xdigit:]]+]]"
// FIXME: This should be empty.
// CHECK-NEXT:         "cas-include-tree-id": "{{.*}}"
// CHECK-NEXT:         "clang-context-hash": "{{.*}}"
// FIXME: This should be empty.
// CHECK-NEXT:         "clang-module-deps": [
// CHECK-NEXT:           {
// CHECK-NEXT:             "context-hash": "{{.*}}
// CHECK-NEXT:             "module-name": "Mod"
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:         "command-line": [
// CHECK-NEXT:           "-cc1"
// CHECK:                "-o"
// CHECK-NEXT:           "[[PREFIX]]/tu.s"
// CHECK:                "-fcas-input-file-cache-key"
// CHECK-NEXT:           "[[COMPILER_CACHE_KEY]]"
// CHECK:                "-S"
// CHECK:                "-x"
// CHECK-NEXT:           "ir"
// CHECK:              ]
// CHECK:              "input-file": "[[PREFIX]]{{.}}tu.c"
// CHECK-NEXT:       }
// CHECK-NEXT:       {
// FIXME: This should be empty.
// CHECK-NEXT:         "cas-include-tree-id": "{{.*}}"
// CHECK-NEXT:         "clang-context-hash": "{{.*}}"
// FIXME: This should be empty.
// CHECK-NEXT:         "clang-module-deps": [
// CHECK-NEXT:           {
// CHECK:                  "module-name": "Mod"
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:         "command-line": [
// CHECK-NEXT:           "-cc1as"
// CHECK:                "-o"
// CHECK-NEXT:           "[[PREFIX]]/tu.o"
// FIXME: The integrated assembler should support caching too.
// CHECK:                "[[PREFIX]]/tu.s"
// CHECK:              ]
// CHECK:              "input-file": "[[PREFIX]]/tu.c"
// CHECK-NEXT:       }
// CHECK-NEXT:     ]
// CHECK-NEXT:   }
// CHECK-NEXT: ]

//--- module.h
void bar(void);

//--- module.modulemap
module Mod { header "module.h" }

//--- tu_define_foo_0.c
#include "module.h"
#define FOO 0
void tu_save_temps(void) { bar(); }

//--- tu_define_foo_1.c
#include "module.h"
#define FOO 1
void tu_save_temps(void) { bar(); }

// REQUIRES: shell

// RUN: rm -rf %t && mkdir %t
// RUN: echo "#!/bin/sh"              > %t/clang
// RUN: echo "echo run some compiler with opts \$*" >> %t/clang
// RUN: chmod +x %t/clang
// RUN: ln -s %clang %t/clang-symlink-outside-bindir

// 'clang-cache' launcher invokes itself, enables caching.
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %clang -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANG -DPREFIX=%t
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %clang++ -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANGPP -DPREFIX=%t
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %t/clang-symlink-outside-bindir -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANG -DPREFIX=%t
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas PATH="%t:$PATH" clang-cache clang-symlink-outside-bindir -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANG -DPREFIX=%t

// CLANG: "-cc1depscan" "-fdepscan=inline"
// CLANG: "-fcas-path" "[[PREFIX]]/cas" "-fcas-token-cache" "-greproducible"
// CLANG: "-x" "c"

// CLANGPP: "-cc1depscan" "-fdepscan=inline"
// CLANGPP: "-fcas-path" "[[PREFIX]]/cas" "-fcas-token-cache" "-greproducible"
// CLANGPP: "-x" "c++"

// 'clang-cache' launcher invokes a different clang, does normal non-caching launch.
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %t/clang -c %s -o %t.o 2>&1 | FileCheck %s -check-prefix=OTHERCLANG -DSRC=%s -DPREFIX=%t
// OTHERCLANG: warning: clang-cache invokes a different clang binary than itself, it will perform a normal non-caching invocation of the compiler
// OTHERCLANG-NEXT: run some compiler with opts -c [[SRC]] -o [[PREFIX]].o

// RUN: not clang-cache %t/nonexistent -c %s -o %t.o 2>&1 | FileCheck %s -check-prefix=NONEXISTENT
// NONEXISTENT: error: clang-cache failed to execute compiler

// RUN: not clang-cache 2>&1 | FileCheck %s -check-prefix=NOCOMMAND
// NOCOMMAND: error: missing compiler command for clang-cache

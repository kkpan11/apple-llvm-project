// REQUIRES: shell

// RUN: rm -rf %t && mkdir %t
// RUN: echo "#!/bin/sh"              > %t/clang
// RUN: echo "echo 'run some clang'" >> %t/clang
// RUN: chmod +x %t/clang
// RUN: ln -s %clang %t/clang-symlink-outside-bindir

// 'clang-cache' launcher invokes itself, enables caching.
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %clang -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANG -DPREFIX=%t
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %clang++ -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANGPP -DPREFIX=%t
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %t/clang-symlink-outside-bindir -c %s -o %t.o -### 2>&1 | FileCheck %s -check-prefix=CLANG -DPREFIX=%t

// 'clang-cache' launcher invokes a different clang, does normal non-caching launch.
// RUN: env CLANG_CACHE_CAS_PATH=%t/cas clang-cache %t/clang -c %s -o %t.o 2>&1 | FileCheck %s -check-prefix=OTHERCLANG

// CLANG: "-cc1depscan" "-fdepscan=inline"
// CLANG: "-fcas-path" "[[PREFIX]]/cas" "-fcas-token-cache" "-greproducible"
// CLANG: "-x" "c"

// CLANGPP: "-cc1depscan" "-fdepscan=inline"
// CLANGPP: "-fcas-path" "[[PREFIX]]/cas" "-fcas-token-cache" "-greproducible"
// CLANGPP: "-x" "c++"

// OTHERCLANG: warning: clang-cache invokes a different clang binary than itself, it will perform a normal non-caching invocation of the compiler
// OTHERCLANG-NEXT: run some clang

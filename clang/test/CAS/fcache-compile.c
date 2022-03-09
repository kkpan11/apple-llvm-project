// RUN: rm -rf %t && mkdir -p %t
// RUN: llvm-cas --cas %t/cas --ingest --data %s > %t/casid
//
// RUN: %clang -cc1 -triple x86_64-apple-macos11 \
// RUN:   -fcas-builtin-path %t/cas -fcas-fs @%t/casid -fexperimental-cache-compile \
// RUN:   -Rcache-compile-hit -emit-obj -o %t/output.o 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CACHE-MISS
// RUN: ls %t/output.o && rm %t/output.o
// RUN: %clang -cc1 -triple x86_64-apple-macos11 \
// RUN:   -fcas-builtin-path %t/cas -fcas-fs @%t/casid -fexperimental-cache-compile \
// RUN:   -Rcache-compile-hit -emit-obj -o %t/output.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CACHE-HIT
// RUN: ls %t/output.o && rm %t/output.o
// RUN: cd %t
// RUN: %clang -cc1 -triple x86_64-apple-macos11 \
// RUN:   -fcas-builtin-path %t/cas -fcas-fs @%t/casid -fexperimental-cache-compile \
// RUN:   -Rcache-compile-hit -emit-obj -o output.o 2>&1 \
// RUN:   | FileCheck %s --allow-empty --check-prefix=CACHE-MISS
// RUN: ls %t/output.o && rm %t/output.o
// RUN: %clang -cc1 -triple x86_64-apple-macos11 \
// RUN:   -fcas-builtin-path %t/cas -fcas-fs @%t/casid -fexperimental-cache-compile \
// RUN:   -Rcache-compile-hit -emit-obj -o output.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CACHE-HIT
// RUN: ls %t/output.o
//
// CACHE-HIT: remark: compile cache hit
// CACHE-MISS-NOT: remark: compile cache hit

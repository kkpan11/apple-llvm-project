// Check that -cc1 closes the CAS databases with -disable-free, which the
// driver always passes to -cc1.

// `OnDiskCASLogger` is not enabled on Windows,
// see `UnifiedOnDiskCache::open()` in `llvm/lib/CAS/UnifiedOnDiskCache.cpp`.
// UNSUPPORTED: system-windows

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -cc1depscan -fdepscan=inline -o %t.rsp -cc1-args \
// RUN:   -cc1 -triple x86_64-apple-macos11 \
// RUN:   -fcas-path %t/cas -emit-obj %s -o %t/output.o
// RUN: env LLVM_CAS_LOG=1 LLVM_CAS_DISABLE_VALIDATION=1 %clang @%t.rsp \
// RUN:   -disable-free -fcache-compile-job -Rcompile-job-cache 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CACHE-MISS
// RUN: FileCheck %s --input-file %t/cas/v1.log

// CACHE-MISS: remark: compile job cache miss

// CHECK: [[PID:[0-9]+]] {{[0-9]+}}: mmap '{{.*}}index.v{{[0-9]+}}'
// CHECK-DAG: [[PID]] {{[0-9]+}}: close mmap '{{.*}}index.v{{[0-9]+}}'
// CHECK-DAG: [[PID]] {{[0-9]+}}: close mmap '{{.*}}data.v{{[0-9]+}}'
// CHECK-DAG: [[PID]] {{[0-9]+}}: close mmap '{{.*}}actions.v{{[0-9]+}}'

void test(void) {}

// Check that a depscan daemon spawned on demand can serve a compilation.
// LLVM_CACHE_BUILD_SESSION_ID is what asks for one; the launcher turns it into
// -fdepscan=daemon -fdepscan-share-identifier=<id>, and the client locates the
// daemon under the system temporary directory before connecting to it.

// REQUIRES: ondisk_cas
// REQUIRES: clang-cc1daemon

// RUN: rm -rf %t && mkdir -p %t

// RUN: env LLVM_CACHE_CAS_PATH=%t/cas LLVM_CACHE_BUILD_SESSION_ID=%basename_t %clang-cache %clang -c %s -o %t/test.o -Xclang -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-MISS
// RUN: env LLVM_CACHE_CAS_PATH=%t/cas LLVM_CACHE_BUILD_SESSION_ID=%basename_t %clang-cache %clang -c %s -o %t/test.o -Xclang -Rcompile-job-cache 2>&1 | FileCheck %s -check-prefix=CACHE-HIT

// CACHE-MISS: remark: compile job cache miss
// CACHE-HIT: remark: compile job cache hit

int main() { return 0; }

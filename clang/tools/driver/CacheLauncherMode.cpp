//===-- CacheLauncherMode.cpp - clang-cache driver mode -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CacheLauncherMode.h"
#include "clang/Basic/DiagnosticCAS.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

using namespace clang;

static bool isSameProgram(const char *clangCachePath,
                          const char *compilerPath) {
  // Fast path check, see if they have the same parent path.
  if (llvm::sys::path::parent_path(clangCachePath) ==
      llvm::sys::path::parent_path(compilerPath))
    return true;
  // Check the file status IDs;
  llvm::sys::fs::file_status CacheStat, CompilerStat;
  if (llvm::sys::fs::status(clangCachePath, CacheStat))
    return false;
  if (llvm::sys::fs::status(compilerPath, CompilerStat))
    return false;
  return CacheStat.getUniqueID() == CompilerStat.getUniqueID();
}

Optional<int>
clang::handleClangCacheInvocation(SmallVectorImpl<const char *> &Args,
                                  llvm::StringSaver &Saver) {
  assert(Args.size() >= 2);
  const char *clangCachePath = Args.front();
  // Drop initial '/path/to/clang-cache' program name.
  Args.erase(Args.begin());
  const char *compilerPath = Args.front();

  if (isSameProgram(clangCachePath, compilerPath)) {
    if (const char *CASPath = ::getenv("CLANG_CACHE_CAS_PATH")) {
      Args.append({"-Xclang", "-fcas-path", "-Xclang", CASPath});
    }
    // FIXME: Add capability to connect to a depscan daemon via 'clang-cache'
    // launcher.
    Args.append(
        {"-fdepscan=inline", "-greproducible", "-Xclang", "-fcas-token-cache"});
    return None;
  }

  // FIXME: If it's invoking a different clang binary determine whether that
  // clang supports the caching options, don't immediately give up on caching.

  // Not invoking same clang binary, do a normal invocation without changing
  // arguments, but warn because this may be unexpected to the user.
  auto DiagsConsumer = std::make_unique<TextDiagnosticPrinter>(
      llvm::errs(), new DiagnosticOptions(), false);
  DiagnosticsEngine Diags(new DiagnosticIDs(), new DiagnosticOptions());
  Diags.setClient(DiagsConsumer.get(), /*ShouldOwnClient=*/false);
  Diags.Report(diag::warn_clang_cache_disabled_caching);

  SmallVector<StringRef, 128> RefArgs;
  RefArgs.reserve(Args.size());
  for (const char *Arg : ArrayRef<const char *>(Args).drop_front()) {
    RefArgs.push_back(Arg);
  }
  return llvm::sys::ExecuteAndWait(compilerPath, RefArgs);
}

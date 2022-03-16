//===- CASOptions.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CAS/CASOptions.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticCAS.h"
#include "llvm/CAS/CASDB.h"

using namespace clang;
using namespace llvm::cas;

static std::shared_ptr<llvm::cas::CASDB>
createCAS(const CASConfiguration &Config, DiagnosticsEngine &Diags,
          bool CreateEmptyCASOnFailure) {
  if (Config.CASPath.empty())
    return llvm::cas::createInMemoryCAS();

  // Compute the path.
  SmallString<128> Storage;
  StringRef Path = Config.CASPath;
  if (Path == "auto") {
    llvm::cas::getDefaultOnDiskCASPath(Storage);
    Path = Storage;
  }

  // FIXME: Pass on the actual error from the CAS.
  if (auto MaybeCAS =
          llvm::expectedToOptional(llvm::cas::createOnDiskCAS(Path)))
    return std::move(*MaybeCAS);
  Diags.Report(diag::err_builtin_cas_cannot_be_initialized) << Path;
  return llvm::cas::createInMemoryCAS();
}

std::shared_ptr<llvm::cas::CASDB>
CASOptions::getOrCreateCAS(DiagnosticsEngine &Diags,
                           bool CreateEmptyCASOnFailure) const {
  if (!Cache.CAS || *this != Cache.Config) {
    Cache.Config = *this;
    Cache.CAS = createCAS(Cache.Config, Diags, CreateEmptyCASOnFailure);
  }

  return Cache.CAS;
}

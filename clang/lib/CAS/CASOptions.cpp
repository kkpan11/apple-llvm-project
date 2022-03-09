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
  if (Config.BuiltinPath.empty())
    return llvm::cas::createInMemoryCAS();

  // FIXME: Pass on the actual error from the CAS.
  if (auto MaybeCAS = llvm::expectedToOptional(
          llvm::cas::createOnDiskCAS(Config.BuiltinPath)))
    return std::move(*MaybeCAS);
  Diags.Report(diag::err_builtin_cas_cannot_be_initialized)
      << Config.BuiltinPath;
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

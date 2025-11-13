//===- CachingActions.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_CACHINGACTIONS_H
#define LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_CACHINGACTIONS_H

#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"

namespace clang::tooling::dependencies {

std::unique_ptr<DependencyActionController>
createIncludeTreeActionController(LookupModuleOutputCallback LookupModuleOutput,
                                  std::shared_ptr<cas::ObjectStore> DB);

} // namespace clang::tooling::dependencies
#endif // LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_CACHINGACTIONS_H

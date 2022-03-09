//===- CASOptions.h - Options for configuring the CAS -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the clang::CASOptions interface.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CAS_CASOPTIONS_H
#define LLVM_CLANG_CAS_CASOPTIONS_H

#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace cas {
class CASDB;
} // end namespace cas
} // end namespace llvm

namespace clang {

class DiagnosticsEngine;

/// Base class for options configuring which CAS to use. Separated for the
/// fields where we don't need special move/copy logic.
///
/// TODO: Add appropriate options once we support plugins.
class CASConfiguration {
public:
  enum CASKind {
    InMemoryCAS,
    OnDiskCAS,
  };

  /// Kind of CAS to use.
  CASKind getKind() const {
    return BuiltinPath.empty() ? InMemoryCAS : OnDiskCAS;
  }

  /// For \a OnDiskCAS, a path on-disk for a persistent backing store. This is
  /// optional, although \a CASFileSystemRootID is unlikely to work.
  ///
  /// "-" indicates the compiler's default location.
  std::string BuiltinPath;

  friend bool operator==(const CASConfiguration &LHS,
                         const CASConfiguration &RHS) {
    return LHS.BuiltinPath == RHS.BuiltinPath;
  }
  friend bool operator!=(const CASConfiguration &LHS,
                         const CASConfiguration &RHS) {
    return !(LHS == RHS);
  }
};

/// Options configuring which CAS to use. User-accessible fields should be
/// defined in CASConfiguration to enable caching a CAS instance.
class CASOptions : public CASConfiguration {
public:
  /// Get a CAS defined by the options above. Future calls will return the same
  /// CAS instance... unless the configuration has changed, in which case a new
  /// one will be created.
  std::shared_ptr<llvm::cas::CASDB>
  getOrCreateCAS(DiagnosticsEngine &Diags,
                 bool CreateEmptyCASOnFailure = false) const;

private:
  struct CachedCAS {
    /// A cached CAS instance.
    std::shared_ptr<llvm::cas::CASDB> CAS;

    /// Remember how the CAS was created.
    CASConfiguration Config;
  };
  mutable CachedCAS Cache;
};

} // end namespace clang

#endif // LLVM_CLANG_CAS_CASOPTIONS_H

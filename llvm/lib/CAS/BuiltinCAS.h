//===- BuiltinCAS.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CAS_BUILTINCAS_H
#define LLVM_LIB_CAS_BUILTINCAS_H

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SHA1.h"
#include <cstddef>

namespace llvm {
namespace cas {
namespace builtin {

/// Dereference the inner value in \p E, adding an Optional to the outside.
/// Useful for stripping an inner Optional in return chaining.
///
/// \code
/// Expected<Optional<SomeType>> f1(...);
///
/// Expected<SomeType> f2(...) {
///   if (Optional<Expected<NoneType>> E = transpose(f1()))
///     return std::move(*E);
///
///   // Deal with None...
/// }
/// \endcode
template <class T>
inline Optional<Expected<T>> transpose(Expected<Optional<T>> E) {
  if (!E)
    return Expected<T>(E.takeError());
  if (*E)
    return Expected<T>(std::move(**E));
  return None;
}

/// Dereference the inner value in \p E, generating an error on failure.
///
/// \code
/// Expected<Optional<SomeType>> f1(...);
///
/// Expected<SomeType> f2(...) {
///   if (Optional<Expected<NoneType>> E = dereferenceValue(f1()))
///     return std::move(*E);
///
///   // Deal with None...
/// }
/// \endcode
template <class T>
inline Expected<T> dereferenceValue(Expected<Optional<T>> E,
                                    function_ref<Error()> OnNone) {
  if (Optional<Expected<T>> MaybeExpected = transpose(std::move(E)))
    return std::move(*MaybeExpected);
  return OnNone();
}

/// If \p E and \c *E, move \c **E into \p Sink.
///
/// Enables expected and optional chaining in one statement:
///
/// \code
/// Expected<Optional<Type1>> f1(...);
///
/// Expected<Optional<Type2>> f2(...) {
///   SomeType V;
///   if (Optional<Expected<NoneType>> E = moveValueInto(f1(), V))
///     return std::move(*E);
///
///   // Deal with value...
/// }
/// \endcode
template <class T, class SinkT>
inline Optional<Expected<NoneType>>
moveValueInto(Expected<Optional<T>> ExpectedOptional, SinkT &Sink) {
  if (!ExpectedOptional)
    return Expected<NoneType>(ExpectedOptional.takeError());
  if (!*ExpectedOptional)
    return Expected<NoneType>(None);
  Sink = std::move(*ExpectedOptional);
  return None;
}

inline StringRef toStringRef(ArrayRef<char> Data) {
  return StringRef(Data.data(), Data.size());
}

inline ArrayRef<char> toArrayRef(StringRef Data) {
  return ArrayRef<char>(Data.data(), Data.size());
}

/// Current hash type for the internal CAS.
using HasherT = SHA1;
using HashType = decltype(HasherT::hash(std::declval<ArrayRef<uint8_t> &>()));

class BuiltinCAS : public CASDB {
  void printIDImpl(raw_ostream &OS, const CASID &ID) const final;

public:
  StringRef getHashSchemaIdentifier() const final {
    return "llvm.cas.builtin.v1[SHA1]";
  }

  Expected<CASID> parseID(StringRef Reference) final;

  bool isKnownObject(CASID ID) final { return bool(getObjectKind(ID)); }

  virtual Expected<CASID> parseIDImpl(ArrayRef<uint8_t> Hash) = 0;

  Expected<TreeProxy> createTree(ArrayRef<NamedTreeEntry> Entries) final;
  virtual Expected<TreeProxy>
  createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                 ArrayRef<NamedTreeEntry> SortedEntries) = 0;

  Expected<NodeProxy> createNode(ArrayRef<CASID> References,
                                 StringRef Data) final {
    return createNode(References, toArrayRef(Data));
  }
  Expected<NodeProxy> createNode(ArrayRef<CASID> References,
                                 ArrayRef<char> Data);
  virtual Expected<NodeProxy> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                             ArrayRef<CASID> References,
                                             ArrayRef<char> Data) = 0;

  Expected<BlobProxy>
  createBlobFromOpenFileImpl(sys::fs::file_t FD,
                             Optional<sys::fs::file_status> Status) override;
  virtual Expected<BlobProxy>
  createBlobFromNullTerminatedRegion(ArrayRef<uint8_t> ComputedHash,
                                     sys::fs::mapped_file_region Map) {
    return createBlobImpl(ComputedHash, makeArrayRef(Map.data(), Map.size()));
  }

  Expected<BlobProxy> createBlob(StringRef Data) final {
    return createBlob(makeArrayRef(Data.data(), Data.size()));
  }
  Expected<BlobProxy> createBlob(ArrayRef<char> Data);
  virtual Expected<BlobProxy> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                             ArrayRef<char> Data) = 0;

  static StringRef getKindName(ObjectKind Kind) {
    switch (Kind) {
    case ObjectKind::Blob:
      return "blob";
    case ObjectKind::Node:
      return "node";
    case ObjectKind::Tree:
      return "tree";
    }
  }

  Error createUnknownObjectError(CASID ID) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "unknown object '" + ID.toString() + "'");
  }

  Error createCorruptObjectError(CASID ID) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "corrupt object '" + ID.toString() + "'");
  }

  Error createInvalidObjectError(CASID ID, ObjectKind Kind) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid object '" + ID.toString() +
                                 "' for kind '" + getKindName(Kind) + "'");
  }

  /// FIXME: This should not use Error.
  Error createResultCacheMissError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "no result for '" + Input.toString() + "'");
  }

  Error createResultCachePoisonedError(CASID Input, CASID Output,
                                       CASID ExistingOutput) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "cache poisoned for '" + Input.toString() +
                                 "' (new='" + Output.toString() +
                                 "' vs. existing '" +
                                 ExistingOutput.toString() + "')");
  }

  Error createResultCacheCorruptError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "result cache corrupt for '" + Input.toString() +
                                 "'");
  }
};

} // end namespace builtin
} // end namespace cas
} // end namespace llvm

#endif // LLVM_LIB_CAS_BUILTINCAS_H

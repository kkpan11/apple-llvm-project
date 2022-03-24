//===- BuiltinCAS.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BuiltinCAS.h"
#include "llvm/CAS/BuiltinObjectHasher.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"

using namespace llvm;
using namespace llvm::cas;
using namespace llvm::cas::builtin;

static StringRef getCASIDPrefix() { return "llvmcas://"; }

static void extractPrintableHash(CASID ID, SmallVectorImpl<char> &Dest) {
  ArrayRef<uint8_t> RawHash = ID.getHash();
  assert(Dest.empty());
  assert(RawHash.size() == 20);
  Dest.reserve(40);
  auto ToChar = [](uint8_t Bit) {
    if (Bit < 10)
      return '0' + Bit;
    return Bit - 10 + 'a';
  };
  for (uint8_t Bit : RawHash) {
    uint8_t High = Bit >> 4;
    uint8_t Low = Bit & 0xf;
    Dest.push_back(ToChar(High));
    Dest.push_back(ToChar(Low));
  }
}

static HashType stringToHash(StringRef Chars) {
  auto FromChar = [](char Ch) -> unsigned {
    if (Ch >= '0' && Ch <= '9')
      return Ch - '0';
    assert(Ch >= 'a');
    assert(Ch <= 'f');
    return Ch - 'a' + 10;
  };

  HashType Hash;
  assert(Chars.size() == sizeof(Hash) * 2);
  for (int I = 0, E = sizeof(Hash); I != E; ++I) {
    uint8_t High = FromChar(Chars[I * 2]);
    uint8_t Low = FromChar(Chars[I * 2 + 1]);
    Hash[I] = (High << 4) | Low;
  }
  return Hash;
}

Expected<CASID> BuiltinCAS::parseID(StringRef Reference) {
  if (!Reference.consume_front(getCASIDPrefix()))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid cas-id '" + Reference + "'");

  // FIXME: Allow shortened references?
  if (Reference.size() != 2 * sizeof(HashType))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "wrong size for cas-id hash '" + Reference + "'");

  // FIXME: Take parsing as a hint that the ID will be loaded and do a look up
  // of blobs and trees, rather than always allocating space for a hash.
  return parseIDImpl(stringToHash(Reference));
}

void BuiltinCAS::printIDImpl(raw_ostream &OS, const CASID &ID) const {
  assert(&ID.getContext() == this);
  assert(ID.getHash().size() == sizeof(HashType));

  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  OS << getCASIDPrefix() << Hash;
}

Expected<BlobProxy> BuiltinCAS::createBlob(ArrayRef<char> Data) {
  return createBlobImpl(BuiltinObjectHasher<HasherT>::hashBlob(Data), Data);
}

static size_t getPageSize() {
  static int PageSize = sys::Process::getPageSizeEstimate();
  return PageSize;
}

Expected<BlobProxy>
BuiltinCAS::createBlobFromOpenFileImpl(sys::fs::file_t FD,
                                       Optional<sys::fs::file_status> Status) {
  int PageSize = getPageSize();

  if (!Status) {
    Status.emplace();
    if (std::error_code EC = sys::fs::status(FD, *Status))
      return errorCodeToError(EC);
  }

  constexpr size_t MinMappedSize = 4 * 4096;
  auto readWithStream = [&]() -> Expected<BlobProxy> {
    SmallString<MinMappedSize * 2> Data;
    if (Error E = sys::fs::readNativeFileToEOF(FD, Data, MinMappedSize))
      return std::move(E);
    return createBlob(makeArrayRef(Data.data(), Data.size()));
  };

  // Check whether we can trust the size from stat.
  if (Status->type() != sys::fs::file_type::regular_file &&
      Status->type() != sys::fs::file_type::block_file)
    return readWithStream();

  if (Status->getSize() < MinMappedSize)
    return readWithStream();

  std::error_code EC;
  sys::fs::mapped_file_region Map(FD, sys::fs::mapped_file_region::readonly,
                                  Status->getSize(),
                                  /*offset=*/0, EC);
  if (EC)
    return errorCodeToError(EC);

  // If the file is guaranteed to be null-terminated, use it directly. Note
  // that the file size may have changed from ::stat if this file is volatile,
  // so we need to check for an actual null character at the end.
  ArrayRef<char> Data(Map.data(), Map.size());
  HashType ComputedHash = BuiltinObjectHasher<HasherT>::hashBlob(Data);
  if (!isAligned(Align(PageSize), Data.size()) && Data.end()[0] == 0)
    return createBlobFromNullTerminatedRegion(ComputedHash, std::move(Map));
  return createBlobImpl(ComputedHash, Data);
}

Expected<TreeProxy> BuiltinCAS::createTree(ArrayRef<NamedTreeEntry> Entries) {
  // Ensure a stable order for tree entries and ignore name collisions.
  SmallVector<NamedTreeEntry> Sorted(Entries.begin(), Entries.end());
  std::stable_sort(Sorted.begin(), Sorted.end());
  Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());

  return createTreeImpl(
      BuiltinObjectHasher<HasherT>::hashTree(Sorted), Sorted);
}

Expected<NodeProxy> BuiltinCAS::createNode(ArrayRef<CASID> References,
                                           ArrayRef<char> Data) {
  return createNodeImpl(BuiltinObjectHasher<HasherT>::hashNode(References, Data),
                        References, Data);
}

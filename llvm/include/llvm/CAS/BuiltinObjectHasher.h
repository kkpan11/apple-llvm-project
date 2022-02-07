//===- BuiltinObjectHasher.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_BUILTINOBJECTHASHER_H
#define LLVM_CAS_BUILTINOBJECTHASHER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

namespace llvm {
namespace cas {

enum class StableObjectKind : uint8_t {
  Node = 1,
  Blob = 2,
  Tree = 3,
  String = 4,
};

enum class StableTreeEntryKind : uint8_t {
  Tree = 1,
  Regular = 2,
  Executable = 3,
  Symlink = 4,
};

static StableTreeEntryKind getStableKind(TreeEntry::EntryKind Kind) {
  switch (Kind) {
  case TreeEntry::Tree:
    return StableTreeEntryKind::Tree;
  case TreeEntry::Regular:
    return StableTreeEntryKind::Regular;
  case TreeEntry::Executable:
    return StableTreeEntryKind::Executable;
  case TreeEntry::Symlink:
    return StableTreeEntryKind::Symlink;
  }
}

static TreeEntry::EntryKind getUnstableKind(StableTreeEntryKind Kind) {
  switch (Kind) {
  case StableTreeEntryKind::Tree:
    return TreeEntry::Tree;
  case StableTreeEntryKind::Regular:
    return TreeEntry::Regular;
  case StableTreeEntryKind::Executable:
    return TreeEntry::Executable;
  case   StableTreeEntryKind::Symlink:
    return TreeEntry::Symlink;
  }
}

static StableObjectKind getStableKind(StableObjectKind Kind) { return Kind; }
static StableObjectKind getStableKind(ObjectKind Kind) {
  switch (Kind) {
  case ObjectKind::Blob:
    return StableObjectKind::Blob;
  case ObjectKind::Node:
    return StableObjectKind::Node;
  case ObjectKind::Tree:
    return StableObjectKind::Tree;
  }
}

class BuiltinObjectHasherBase {
public:
  template <class KindT>
  static std::array<uint8_t, sizeof(KindT)> serializeKind(KindT Kind) {
    auto StableKind = getStableKind(Kind);
    std::array<uint8_t, sizeof(KindT)> Bytes;
    llvm::support::endian::write(Bytes.data(), StableKind, support::endianness::little);
    return Bytes;
  }
};

template <class HasherT> class BuiltinObjectHasher : public BuiltinObjectHasherBase {
public:
  using HashT = decltype(HasherT::hash(std::declval<ArrayRef<uint8_t>&>()));

protected:
  ~BuiltinObjectHasher() { assert(!Hasher); }

  void start(ObjectKind Kind) { start(getStableKind(Kind)); }
  void start(StableObjectKind Kind) {
    assert(!Hasher);
    Hasher.emplace();
    updateKind(Kind);
  }

  HashT finish() {
    assert(Hasher);
    StringRef Final = Hasher->final();
    auto *Begin = reinterpret_cast<const uint8_t *>(Final.begin());
    HashT Hash;
    std::copy(Begin, Begin + Final.size(), Hash.data());
    Hasher.reset();
    return Hash;
  }

  template <class KindT> void updateKind(KindT Kind) {
    Hasher->update(serializeKind(Kind));
  }

  void updateString(StringRef String) {
    updateArray(makeArrayRef(String.data(), String.size()));
  }

  void updateHash(ArrayRef<uint8_t> Hash) {
    assert(Hash.size() == sizeof(HashT));
    Hasher->update(Hash);
  }

  void updateArray(ArrayRef<uint8_t> Bytes) {
    updateSize(Bytes.size());
    Hasher->update(Bytes);
  }

  void updateArray(ArrayRef<char> Bytes) {
    updateArray(makeArrayRef(reinterpret_cast<const uint8_t *>(Bytes.data()),
                             Bytes.size()));
  }

  void updateSize(uint64_t Size) {
    std::array<uint8_t, sizeof(uint64_t)> Bytes;
    llvm::support::endian::write(Bytes.data(), Size, support::endianness::little);
    Hasher->update(Bytes);
  }

private:
  Optional<HasherT> Hasher;
};

template <class HasherT> class BuiltinBlobHasher : public BuiltinObjectHasher<HasherT> {
public:
  auto hash(ArrayRef<char> Data) {
    this->start(ObjectKind::Blob);
    this->updateArray(Data);
    return this->finish();
  }
  auto hash(StringRef Data) {
    return hash(makeArrayRef(Data.data(), Data.size()));
  }
};

template <class HasherT> class BuiltinStringHasher : public BuiltinObjectHasher<HasherT> {
public:
  auto hash(StringRef Data) {
    this->start(StableObjectKind::String);
    this->updateString(Data);
    return this->finish();
  }
};

template <class HasherT> class BuiltinNodeHasher : public BuiltinObjectHasher<HasherT> {
public:
  void start(size_t NumRefs) {
    BuiltinNodeHasher::BuiltinObjectHasher::start(ObjectKind::Node);
    this->updateSize(NumRefs);
    RemainingRefs = NumRefs;
  }

  void updateRef(ArrayRef<uint8_t> Hash) {
    assert(RemainingRefs && "Expected fewer refs");
    --RemainingRefs;
    this->updateHash(Hash);
  }

  auto finish(ArrayRef<char> Data) {
    assert(!RemainingRefs && "Expected more refs");
    this->updateArray(Data);
    return BuiltinNodeHasher::BuiltinObjectHasher::finish();
  }

private:
  size_t RemainingRefs = 0;
};

template <class HasherT> class BuiltinTreeHasher
    : public BuiltinObjectHasher<HasherT> {
public:
  void start(size_t Size) {
    BuiltinTreeHasher::BuiltinObjectHasher::start(ObjectKind::Tree);
    this->updateSize(Size);
    RemainingEntries = Size;
  }

  void updateEntry(ArrayRef<uint8_t> RefHash, StringRef Name,
                   TreeEntry::EntryKind Kind) {
    assert(RemainingEntries && "Expected fewer refs");
    --RemainingEntries;

    this->updateHash(RefHash);
    this->updateString(Name);
    this->updateKind(Kind);
  }

  auto finish() {
    assert(RemainingEntries == 0 && "Expected more refs");
    return BuiltinTreeHasher::BuiltinObjectHasher::finish();
  }

private:
  size_t RemainingEntries = 0;
};

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_BUILTINOBJECTHASHER_H

//===- BuiltinCAS.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/BuiltinObjectHasher.h"
#include "llvm/CAS/HashMappedTrie.h"
#include "llvm/ADT/LazyAtomicPointer.h"
#include "llvm/CAS/LazyMappedFileRegionBumpPtr.h"
#include "llvm/CAS/OnDiskHashMappedTrie.h"
#include "llvm/CAS/ThreadSafeAllocator.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;
using namespace llvm::cas;

namespace {

/// Dereference the inner value in \p E, adding an Optional to the outside.
/// Useful for stripping an inner Optional in return chaining.
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
template <class T> static Optional<Expected<T>>
dereferenceValue(Expected<Optional<T>> E) {
  if (!E)
    return E.takeError();
  if (*E)
    return std::move(**E);
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
template <class T> static Expected<T>
dereferenceValue(Expected<Optional<T>> E, function_ref<Error ()> OnNone) {
  if (Optional<Expected<T>> MaybeExpected = dereferenceValue(std::move(E)))
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
template <class T, class SinkT> static Optional<Expected<NoneType>>
moveValueInto(Expected<Optional<T>> E, SinkT &Sink) {
  if (Optional<Expected<T>> MaybeExpected = dereferenceValue(std::move(E)))
    return MaybeExpected->moveInto(Sink);
  return None;
}

/// Simple thread-safe access.
template <class MapT, size_t NumShards> class ThreadSafeMap {
  static_assert(isPowerOf2_64(NumShards), "Expected power of 2");
public:
  using MapType = MapT;
  template <class ReturnT>
  auto withLock(size_t Shard, function_ref<ReturnT (MapT &)> CB) {
    auto &S = getShard(Shard);
    std::lock_guard<std::mutex> Lock(S.Mutex);
    return CB(S.Map);
  }
  template <class ReturnT>
  auto withLock(size_t Shard, function_ref<ReturnT (const MapT &)> CB) const {
    auto &S = getShard(Shard);
    std::lock_guard<std::mutex> Lock(S.Mutex);
    return CB(S.Map);
  }

private:
  struct Shard {
    MapT Map;
    mutable std::mutex Mutex;
  };
  Shard &getShard(size_t S) {
    return const_cast<Shard &>(
        const_cast<const ThreadSafeMap *>(this)->getShard(S));
  }
  const Shard &getShard(size_t S) const {
    static_assert(NumShards <= 256, "Expected only 8 bits of shard");
    return Shards[S % NumShards];
  }

  Shard Shards[NumShards];
};

} // end anonymous namespace

namespace {

template <typename T> static T reportAsFatalIfError(Expected<T> ValOrErr) {
  if (!ValOrErr)
    report_fatal_error(ValOrErr.takeError());
  return std::move(*ValOrErr);
}

static void reportAsFatalIfError(Error E) {
  if (E)
    report_fatal_error(std::move(E));
}

class BuiltinCAS : public CASDB {
public:
  Expected<CASID> parseCASID(StringRef Reference) final;
  Error printCASID(raw_ostream &OS, CASID ID) const final;

  virtual Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) = 0;

  SmallString<64> getPrintedIDOrHash(CASID ID) const;

  static size_t getPageSize() {
    static int PageSize = sys::Process::getPageSizeEstimate();
    return PageSize;
  }

  Expected<TreeRef> createTree(ArrayRef<NamedTreeEntry> Entries) final;
  virtual Expected<TreeRef> createTreeImpl(ArrayRef<uint8_t> ComputedHash, ArrayRef<NamedTreeEntry> SortedEntries) = 0;

  Expected<NodeRef> createNode(ArrayRef<CASID> References,
                               ArrayRef<char> Data) final;
  virtual Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                               ArrayRef<CASID> References, ArrayRef<char> Data) = 0;

  Expected<BlobRef> createBlobFromOpenFileImpl(
      sys::fs::file_t FD, Optional<sys::fs::file_status> Status) override;
  virtual Expected<BlobRef> createBlobFromNullTerminatedRegion(
      ArrayRef<uint8_t> ComputedHash,
      sys::fs::mapped_file_region Map) {
    return createBlobImpl(ComputedHash, makeArrayRef(Map.data(), Map.size()));
  }

  Expected<BlobRef> createBlob(ArrayRef<char> Data) final;
  virtual Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                           ArrayRef<char> Data) = 0;

  static StringRef getKindName(ObjectKind Kind) {
    switch (Kind) {
    case ObjectKind::Blob: return "blob";
    case ObjectKind::Node: return "node";
    case ObjectKind::Tree: return "tree";
    }
  }

  Error createUnknownObjectError(CASID ID) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "unknown object '" + getPrintedIDOrHash(ID) + "'");
  }

  Error createCorruptObjectError(CASID ID) const {
     return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "corrupt object '" + getPrintedIDOrHash(ID) + "'");
   }

  Error createInvalidObjectError(CASID ID, ObjectKind Kind) const {
     return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid object '" + getPrintedIDOrHash(ID) +
                             "' for kind '" + getKindName(Kind) + "'");
  }

  /// FIXME: This should not use Error.
  Error createResultCacheMissError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "no result for '" + getPrintedIDOrHash(Input) + "'");
  }

  Error createResultCachePoisonedError(CASID Input, CASID Output,
                                       CASID ExistingOutput)  const{
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "cache poisoned for '" + getPrintedIDOrHash(Input) + "' (new='" +
                             getPrintedIDOrHash(Output) + "' vs. existing '" +
                             getPrintedIDOrHash(ExistingOutput) + "')");
  }

  Error createResultCacheCorruptError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "result cache corrupt for '" +
                             getPrintedIDOrHash(Input) + "'");
  }
};

/// Current hash type for the internal CAS.
using HasherT = SHA1;
using HashType = decltype(HasherT::hash(std::declval<ArrayRef<uint8_t>&>()));

} // end anonymous namespace

static ArrayRef<uint8_t> bufferAsRawHash(StringRef Bytes) {
  assert(Bytes.size() == sizeof(HashType));
  return ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.begin()),
                 Bytes.size());
}

static StringRef rawHashAsBuffer(ArrayRef<uint8_t> Hash) {
  assert(Hash.size() == sizeof(HashType));
  return StringRef(reinterpret_cast<const char *>(Hash.begin()), Hash.size());
}

static StringRef toStringRef(ArrayRef<char> Data) {
  return StringRef(Data.data(), Data.size());
}

static ArrayRef<char> toArrayRef(StringRef Data) {
  return ArrayRef<char>(Data.data(), Data.size());
}

static HashType makeHash(ArrayRef<uint8_t> Bytes) {
  assert(Bytes.size() == sizeof(HashType));
  HashType Hash;
  ::memcpy(Hash.begin(), Bytes.begin(), Bytes.size());
  return Hash;
}

static HashType makeHash(CASID ID) { return makeHash(ID.getHash()); }

static StringRef getCASIDPrefix() { return "~{CASFS}:"; }

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

SmallString<64> BuiltinCAS::getPrintedIDOrHash(CASID ID) const {
  SmallString<64> Printed;
  if (Error E = getPrintedCASID(ID, Printed)) {
    consumeError(std::move(E));
    extractPrintableHash(ID, Printed);
  }
  return Printed;
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

Expected<CASID> BuiltinCAS::parseCASID(StringRef Reference) {
  if (!Reference.consume_front(getCASIDPrefix()))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid cas-id '" + Reference + "'");

  // FIXME: Allow shortened references?
  if (Reference.size() != 2 * sizeof(HashType))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "wrong size for cas-id hash '" + Reference + "'");

  // FIXME: Take parsing as a hint that the ID will be loaded and do a look up
  // of blobs and trees, rather than always allocating space for a hash.
  return parseCASIDImpl(stringToHash(Reference));
}

Error BuiltinCAS::printCASID(raw_ostream &OS, CASID ID) const {
  if (ID.getHash().size() != sizeof(HashType))
    return errorCodeToError(std::make_error_code(std::errc::invalid_argument));

  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  OS << getCASIDPrefix() << Hash;
  return Error::success();
}

Expected<BlobRef> BuiltinCAS::createBlob(ArrayRef<char> Data) {
  BuiltinBlobHasher<HasherT> Hasher;
  return createBlobImpl(Hasher.hash(Data), Data);
}

Expected<BlobRef>
BuiltinCAS::createBlobFromOpenFileImpl(sys::fs::file_t FD,
                                       Optional<sys::fs::file_status> Status) {
  int PageSize = getPageSize();

  if (!Status) {
    Status.emplace();
    if (std::error_code EC = sys::fs::status(FD, *Status))
      return errorCodeToError(EC);
  }

  constexpr size_t MinMappedSize = 4 * 4096;
  auto readWithStream = [&]() -> Expected<BlobRef> {
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
  sys::fs::mapped_file_region Map(
      FD, sys::fs::mapped_file_region::readonly, Status->getSize(),
      /*offset=*/0, EC);
  if (EC)
    return errorCodeToError(EC);

  // If the file is guaranteed to be null-terminated, use it directly. Note
  // that the file size may have changed from ::stat if this file is volatile,
  // so we need to check for an actual null character at the end.
  ArrayRef<char> Data(Map.data(), Map.size());
  BuiltinBlobHasher<HasherT> Hasher;
  HashType ComputedHash = Hasher.hash(Data);
  if (!isAligned(Align(PageSize), Data.size()) && Data.end()[0] == 0)
    return createBlobFromNullTerminatedRegion(ComputedHash, std::move(Map));
  return createBlobImpl(ComputedHash, Data);
}

Expected<TreeRef> BuiltinCAS::createTree(ArrayRef<NamedTreeEntry> Entries) {
  SmallVector<NamedTreeEntry> Sorted(Entries.begin(), Entries.end());

  // Ensure a stable order for tree entries and ignore name collisions.
  std::stable_sort(Sorted.begin(), Sorted.end());
  Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());

  // Look up the hash in the index, initializing to nullptr if it's new.
  BuiltinTreeHasher<HasherT> Hasher;
  Hasher.start(Sorted.size());
  for (const NamedTreeEntry &E : Sorted)
    Hasher.updateEntry(E.getID().getHash(), E.getName(), E.getKind());
  return createTreeImpl(Hasher.finish(), Sorted);
}

namespace {

class InMemoryObject;
class InMemoryBlob;
class InMemoryNode;
class InMemoryTree;
class InMemorySmallBlob;
class InMemorySmallNode;
class InMemoryRefBlob;
class InMemoryRefNode;
class InMemoryString;

/// Index of referenced IDs (map: Hash -> InMemoryObject*). Uses
/// LazyAtomicPointer to coordinate creation of objects.
using InMemoryIndexT = ThreadSafeHashMappedTrie<
    LazyAtomicPointer<const InMemoryObject>, sizeof(HashType)>;

/// Values in \a InMemoryIndexT. \a InMemoryObject's point at this to access
/// their hash.
using InMemoryIndexValueT = InMemoryIndexT::value_type;

/// String pool.
using InMemoryStringPoolT = ThreadSafeHashMappedTrie<
    LazyAtomicPointer<const InMemoryString>, sizeof(HashType)>;

/// Action cache type (map: Hash -> InMemoryObject*). Always refers to existing
/// objects.
using InMemoryCacheT = ThreadSafeHashMappedTrie<const InMemoryIndexValueT *, sizeof(HashType)>;

class InMemoryObject {
public:
  enum class Kind {
    /// Node with refs and data.
    RefNode,

    /// Blob with data (8-byte alignment guaranteed, null-terminated).
    RefBlob,

    /// Node with refs and data co-allocated.
    InlineNode,

    /// Blob with data (as above) co-allocated.
    InlineBlob,

    /// Tree: custom node. Pairs of refs pointing at target (arbitrary object)
    /// and names (String), and some data to describe the kind of the entry.
    Tree,

    Max = Tree,
  };

  Kind getKind() const { return IndexAndKind.getInt(); }
  const InMemoryIndexValueT &getIndex() const {
    assert(IndexAndKind.getPointer());
    return *IndexAndKind.getPointer();
  }

  ArrayRef<uint8_t> getHash() const { return getIndex().Hash; }

  InMemoryObject() = delete;
  InMemoryObject(InMemoryObject &&) = delete;
  InMemoryObject(const InMemoryObject &) = delete;

protected:
  InMemoryObject(Kind K, const InMemoryIndexValueT &I)
      : IndexAndKind(&I, K) {}

private:
  enum Counts : int {
    NumKindBits = 3,
  };
  PointerIntPair<const InMemoryIndexValueT *, NumKindBits, Kind>
      IndexAndKind;
  static_assert((1U << NumKindBits) <= alignof(InMemoryIndexValueT),
                "Kind will clobber pointer");
  static_assert(((int)Kind::Max >> NumKindBits) == 0, "Kind will be truncated");
};

template <class DerivedT> class GetDataString {
public:
  StringRef getDataString() const {
    ArrayRef<char> Array = static_cast<const DerivedT *>(this)->getData();
    return StringRef(Array.begin(), Array.size());
  }
};

class InMemoryBlob : public InMemoryObject, public GetDataString<InMemoryBlob> {
public:
  inline ArrayRef<char> getData() const;

private:
  using InMemoryObject::InMemoryObject;
};

class InMemoryRefBlob : public InMemoryBlob {
public:
  static constexpr Kind KindValue = Kind::RefBlob;

  ArrayRef<char> getDataImpl() const { return Data; }
  ArrayRef<char> getData() const { return Data; }

  static InMemoryRefBlob &create(
      function_ref<void *(size_t Size)> Allocate,
      const InMemoryIndexValueT &I, ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryRefBlob));
    return *new (Mem) InMemoryRefBlob(I, Data);
  }

private:
  InMemoryRefBlob(const InMemoryIndexValueT &I, ArrayRef<char> Data)
      : InMemoryBlob(KindValue, I), Data(Data) {
    assert(isAddrAligned(Align(8), Data.data()) && "Expected 8-byte alignment");
    assert(*Data.end() == 0 && "Expected null-termination");
  }
  ArrayRef<char> Data;
};

class InMemoryInlineBlob : public InMemoryBlob {
public:
  static constexpr Kind KindValue = Kind::InlineBlob;

  ArrayRef<char> getDataImpl() const {
    return makeArrayRef(reinterpret_cast<const char *>(this + 1), Size);
  }
  ArrayRef<char> getData() const { return getDataImpl(); }

  static InMemoryInlineBlob &create(
      function_ref<void *(size_t Size)> Allocate,
      const InMemoryIndexValueT &I, ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryInlineBlob) + Data.size() + 1);
    return *new (Mem) InMemoryInlineBlob(I, Data);
  }

private:
  InMemoryInlineBlob(const InMemoryIndexValueT &I, ArrayRef<char> Data)
      : InMemoryBlob(KindValue, I), Size(Data.size()) {
    auto *Begin = reinterpret_cast<char *>(this + 1);
    llvm::copy(Data, Begin);
    Begin[Data.size()] = 0;
  }
  size_t Size;
};

class InMemoryNode : public InMemoryObject, public GetDataString<InMemoryNode> {
public:
  inline ArrayRef<char> getData() const;
  inline ArrayRef<const InMemoryObject *> getRefs() const;

private:
  using InMemoryObject::InMemoryObject;
};

class InMemoryRefNode : public InMemoryNode {
public:
  static constexpr Kind KindValue = Kind::RefNode;

  ArrayRef<const InMemoryObject *> getRefsImpl() const { return Refs; }
  ArrayRef<const InMemoryObject *> getRefs() const { return Refs; }
  ArrayRef<char> getDataImpl() const { return Data; }
  ArrayRef<char> getData() const { return Data; }

  static InMemoryRefNode &create(
      function_ref<void *(size_t Size)> Allocate,
      const InMemoryIndexValueT &I,
      ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryRefNode));
    return *new (Mem) InMemoryRefNode(I, Refs, Data);
  }

private:
  InMemoryRefNode(const InMemoryIndexValueT &I,
               ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data)
      : InMemoryNode(KindValue, I), Refs(Refs), Data(Data) {
    assert(isAddrAligned(Align(8), Data.data()) && "Expected 8-byte alignment");
    assert(*Data.end() == 0 && "Expected null-termination");
  }

  ArrayRef<const InMemoryObject *> Refs;
  ArrayRef<char> Data;
};

class InMemoryInlineNode : public InMemoryNode {
public:
  static constexpr Kind KindValue = Kind::InlineNode;

  ArrayRef<const InMemoryObject *> getRefs() const { return getRefsImpl(); }
  ArrayRef<const InMemoryObject *> getRefsImpl() const {
    return makeArrayRef(reinterpret_cast<const InMemoryObject *const *>(this + 1), NumRefs);
  }

  ArrayRef<char> getData() const { return getDataImpl(); }
  ArrayRef<char> getDataImpl() const {
    ArrayRef<const InMemoryObject *> Refs = getRefs();
    return makeArrayRef(reinterpret_cast<const char *>(Refs.data() + Refs.size()),
                        DataSize);
  }

  static InMemoryInlineNode &create(
      function_ref<void *(size_t Size)> Allocate,
      const InMemoryIndexValueT &I,
      ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryInlineNode) +
                         sizeof(uintptr_t) * Refs.size() +
                         Data.size() + 1);
    return *new (Mem) InMemoryInlineNode(I, Refs, Data);
  }

private:
  InMemoryInlineNode(const InMemoryIndexValueT &I,
                    ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data)
      : InMemoryNode(KindValue, I), NumRefs(Refs.size()), DataSize(Data.size()) {
    auto *BeginRefs = reinterpret_cast<const InMemoryObject **>(this + 1);
    llvm::copy(Refs, BeginRefs);
    auto *BeginData = reinterpret_cast<char *>(BeginRefs + NumRefs);
    llvm::copy(Data, BeginData);
    BeginData[Data.size()] = 0;
  }
  uint32_t NumRefs;
  uint32_t DataSize;
};

class InMemoryTree : public InMemoryObject {
public:
  static constexpr Kind KindValue = Kind::Tree;

  struct NamedRef {
    const InMemoryObject *Ref;
    const InMemoryString *Name;
  };

  struct NamedEntry {
    const InMemoryObject *Ref;
    const InMemoryString *Name;
    TreeEntry::EntryKind Kind;
  };

  ArrayRef<NamedRef> getNamedRefs() const {
    return makeArrayRef(reinterpret_cast<const NamedRef *>(this + 1), Size);
  }

  ArrayRef<TreeEntry::EntryKind> getKinds() const {
    ArrayRef<NamedRef> Refs = getNamedRefs();
    return makeArrayRef(reinterpret_cast<const TreeEntry::EntryKind *>(
          Refs.data() + Refs.size()), Size);
  }

  Optional<NamedEntry> find(StringRef Name) const;

  bool empty() const { return !Size; }
  size_t size() const { return Size; }

  NamedEntry operator[](ptrdiff_t I) const {
    assert((size_t)I < size());
    NamedRef NR = getNamedRefs()[I];
    return NamedEntry{NR.Ref, NR.Name, getKinds()[I]};
  }

  static InMemoryTree &create(
      function_ref<void *(size_t Size)> Allocate,
      const InMemoryIndexValueT &I, ArrayRef<NamedEntry> Entries);

private:
  InMemoryTree(const InMemoryIndexValueT &I,
               ArrayRef<NamedEntry> Entries);
  size_t Size;
};

} // end anonymous namespace

namespace llvm {

template <typename T>
struct isa_impl<T, InMemoryObject,
                std::enable_if_t<std::is_base_of<InMemoryObject, T>::value>> {
  static inline bool doit(const InMemoryObject &O) {
    return T::KindValue == O.getKind();
  }
};

template <> struct isa_impl<InMemoryBlob, InMemoryObject> {
  static inline bool doit(const InMemoryObject &O) {
    return isa<InMemoryRefBlob>(O) || isa<InMemorySmallBlob>(O);
  }
};

template <> struct isa_impl<InMemoryNode, InMemoryObject> {
  static inline bool doit(const InMemoryObject &O) {
    return isa<InMemoryRefNode>(O) || isa<InMemorySmallNode>(O);
  }
};

} // end namespace llvm

namespace {

/// Internal string type.
class InMemoryString {
public:
  StringRef get() const {
    return StringRef(reinterpret_cast<const char *>(this + 1), Size);
  }

  static InMemoryString &create(
      function_ref<void *(size_t Size)> Allocate, StringRef String) {
    assert(String.size() <= UINT32_MAX && "Expected strings smaller than 4GB");
    void *Mem = Allocate(sizeof(InMemoryString) + String.size() + 1);
    return *new (Mem) InMemoryString(String);
  }

private:
  InMemoryString(StringRef String) : Size(String.size()) {
    auto *Begin = reinterpret_cast<char *>(this + 1);
    llvm::copy(String, Begin);
    Begin[String.size()] = 0;
  }
  size_t Size;
};

/// In-memory CAS database and action cache (the latter should be separated).
class InMemoryCAS : public BuiltinCAS {
public:
  Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) final;

  Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<char> Data) final;
  Expected<TreeRef> createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<NamedTreeEntry> Entries) final;
  Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<CASID> References,
                                   ArrayRef<char> Data) final;
  bool isKnownObject(CASID ID) final;
  Optional<ObjectKind> getObjectKind(CASID ID) final;

  Expected<BlobRef> createBlobFromNullTerminatedRegion(
      ArrayRef<uint8_t> ComputedHash,
      sys::fs::mapped_file_region Map) override;

  static CASID getID(const InMemoryIndexValueT &I) { return CASID(I.Hash); }
  static CASID getID(const InMemoryObject &O) { return CASID(O.getHash()); }

  Expected<BlobRef> getBlobRef(const InMemoryObject &Object) {
    if (auto *Blob = dyn_cast<InMemoryBlob>(&Object))
      return makeBlobRef(getID(Object), Blob->getDataString());
    return createInvalidObjectError(getID(Object), ObjectKind::Blob);
  }

  Expected<NodeRef> getNodeRef(const InMemoryObject &Object) {
    if (auto *Node = dyn_cast<InMemoryNode>(&Object))
      return makeNodeRef(getID(Object), Node, Node->getRefs().size(),
                         Node->getDataString());
    return createInvalidObjectError(getID(Object), ObjectKind::Node);
  }

  Expected<TreeRef> getTreeRef(const InMemoryObject &Object) {
    if (auto *Tree = dyn_cast<InMemoryTree>(&Object))
      return makeTreeRef(getID(Object), Tree, Tree->size());
    return createInvalidObjectError(getID(Object), ObjectKind::Tree);
  }

  InMemoryIndexValueT &indexHash(ArrayRef<uint8_t> Hash) {
    return *Index.insertLazy(
      Hash, [](auto ValueConstructor) { ValueConstructor.emplace(nullptr); });
  }

  /// TODO: Consider callers to actually do an insert and to return a handle to
  /// the slot in the trie.
  const InMemoryObject *getObject(CASID ID) const {
    if (InMemoryIndexT::const_pointer P = Index.find(ID.getHash()))
      return P->Data;
    return nullptr;
  }

  const InMemoryString &getOrCreateString(StringRef String);
  Expected<BlobRef> getBlob(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getBlobRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Blob);
  }
  Expected<NodeRef> getNode(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getNodeRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Node);
  }
  Expected<TreeRef> getTree(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getTreeRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Tree);
  }
  void print(raw_ostream &OS) const final;

  Expected<CASID> getCachedResult(CASID InputID) final;
  Error putCachedResult(CASID InputID, CASID OutputID) final;

  InMemoryCAS() = default;

private:
  // TreeAPI.
  static NamedTreeEntry makeTreeEntry(const InMemoryTree::NamedEntry &Entry) {
    return NamedTreeEntry(CASID(Entry.Ref->getHash()), Entry.Kind, Entry.Name->get());
  }
  Optional<NamedTreeEntry> lookupInTree(const TreeRef &Handle,
                                        StringRef Name) const final;
  NamedTreeEntry getInTree(const TreeRef &Handle, size_t I) const final;
  Error forEachEntryInTree(
      const TreeRef &Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const final;

  // NodeAPI.
  CASID getReferenceInNode(const NodeRef &Handle, size_t I) const final;
  Error forEachReferenceInNode(const NodeRef &Handle,
                               function_ref<Error(CASID)> Callback) const final;

  /// Index of referenced IDs (map: Hash -> InMemoryObject*). Mapped to nullptr
  /// as a convenient way to store hashes.
  ///
  /// - Insert nullptr on lookups.
  /// - InMemoryObject points back to here.
  InMemoryIndexT Index;

  /// String pool for trees.
  InMemoryStringPoolT StringPool;

  /// Action cache (map: Hash -> InMemoryObject*).
  InMemoryCacheT ActionCache;

  ThreadSafeAllocator<BumpPtrAllocator> Objects;
  ThreadSafeAllocator<BumpPtrAllocator> Strings;
  ThreadSafeAllocator<SpecificBumpPtrAllocator<sys::fs::mapped_file_region>>
      MemoryMaps;
};

} // end anonymous namespace


ArrayRef<char> InMemoryBlob::getData() const {
  if (auto *Derived = dyn_cast<InMemoryRefBlob>(this))
    return Derived->getDataImpl();
  return cast<InMemoryInlineBlob>(this)->getDataImpl();
}

ArrayRef<char> InMemoryNode::getData() const {
  if (auto *Derived = dyn_cast<InMemoryRefNode>(this))
    return Derived->getDataImpl();
  return cast<InMemoryInlineNode>(this)->getDataImpl();
}

ArrayRef<const InMemoryObject *> InMemoryNode::getRefs() const {
  if (auto *Derived = dyn_cast<InMemoryRefNode>(this))
    return Derived->getRefsImpl();
  return cast<InMemoryInlineNode>(this)->getRefsImpl();
}

void InMemoryCAS::print(raw_ostream &OS) const {
  OS << "index: ";
  Index.print(OS);
  OS << "strings: ";
  Index.print(OS);
  OS << "action-cache: ";
  ActionCache.print(OS);
}

InMemoryTree &InMemoryTree::create(
    function_ref<void *(size_t Size)> Allocate,
    const InMemoryIndexValueT &I, ArrayRef<NamedEntry> Entries) {
  void *Mem = Allocate(sizeof(InMemoryTree) +
                       sizeof(NamedRef) * Entries.size() +
                       sizeof(TreeEntry::EntryKind) * Entries.size());
  return *new (Mem) InMemoryTree(I, Entries);
}

InMemoryTree::InMemoryTree(const InMemoryIndexValueT &I,
                           ArrayRef<NamedEntry> Entries)
    : InMemoryObject(KindValue, I), Size(Entries.size()) {
  const InMemoryString *LastName = nullptr;
  auto *Ref = reinterpret_cast<NamedRef *>(this + 1);
  for (const NamedEntry &E : Entries) {
    assert(E.Ref);
    assert(E.Name);
    new (Ref++) NamedRef{E.Ref, E.Name};

    assert((!LastName || LastName->get() < E.Name->get()) &&
           "Expected names to be unique and sorted");
  }
  auto *Entry = reinterpret_cast<NamedEntry *>(Ref);
  for (const NamedEntry &E : Entries)
    new (Entry++) TreeEntry::EntryKind(E.Kind);
}

Optional<InMemoryTree::NamedEntry> InMemoryTree::find(StringRef Name) const {
  auto Compare = [](const NamedRef &LHS, StringRef RHS) {
    return LHS.Name->get().compare(RHS) < 0;
  };

  ArrayRef<NamedRef> Refs = getNamedRefs();
  const NamedRef *I = std::lower_bound(Refs.begin(), Refs.end(), Compare);
  if (I == Refs.end() || I->Name->get() != Name)
    return None;
  return operator[](I - Refs.begin());
}

Expected<BlobRef> InMemoryCAS::createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                              ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);

  // Load or generate.
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryInlineBlob::create(Allocator, I, Data);
  };

  return getBlobRef(I.Data.loadOrGenerate(Generator));
}

Expected<BlobRef> InMemoryCAS::createBlobFromNullTerminatedRegion(
    ArrayRef<uint8_t> ComputedHash,
    sys::fs::mapped_file_region Map) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  ArrayRef<char> Data(Map.data(), Map.size());
  auto &I = indexHash(ComputedHash);

  // Load or generate.
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryRefBlob::create(Allocator, I, Data);
  };
  const InMemoryObject &Object = I.Data.loadOrGenerate(Generator);

  // Save Map if the winning blob uses it.
  if (auto *Blob = dyn_cast<InMemoryRefBlob>(&Object))
    if (Blob->getData().data() == Map.data())
      new (MemoryMaps.Allocate()) sys::fs::mapped_file_region(std::move(Map));

  return getBlobRef(Object);
}

Expected<NodeRef> InMemoryCAS::createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                          ArrayRef<CASID> Refs,
                                          ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);
  if (const InMemoryObject *Node = I.Data.load())
    return getNodeRef(*Node);

  // Create the node.
  SmallVector<const InMemoryObject *> InternalRefs;
  for (CASID ID : Refs) {
    if (const InMemoryObject *Object = getObject(ID))
      InternalRefs.push_back(Object);
    else
      return createInvalidObjectError(ID, ObjectKind::Node);
  }
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryRefNode::create(Allocator, I, InternalRefs, Data);
  };
  return getNodeRef(I.Data.loadOrGenerate(Generator));
}

const InMemoryString &InMemoryCAS::getOrCreateString(StringRef String) {
  BuiltinStringHasher<HasherT> Hasher;
  InMemoryStringPoolT::value_type S = *StringPool.insertLazy(
      Hasher.hash(String), [](auto ValueConstructor) { ValueConstructor.emplace(nullptr); });

  auto Allocator = [&](size_t Size) -> void * {
    return Strings.Allocate(String.size(), 1);
  };
  auto Generator = [&]() -> const InMemoryString * {
    return &InMemoryString::create(Allocator, String);
  };
  return S.Data.loadOrGenerate(Generator);
}

Expected<TreeRef> InMemoryCAS::createTreeImpl(
    ArrayRef<uint8_t> ComputedHash, ArrayRef<NamedTreeEntry> SortedEntries) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);
  if (const InMemoryObject *Tree = I.Data.load())
    return getTreeRef(*Tree);

  // Create the tree.
  SmallVector<InMemoryTree::NamedEntry> InternalEntries;
  for (const NamedTreeEntry &E : SortedEntries) {
    InternalEntries.push_back({getObject(E.getID()),
                               &getOrCreateString(E.getName()),
                               E.getKind()});
    if (!InternalEntries.back().Ref)
      return createUnknownObjectError(E.getID());
  }
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryTree::create(Allocator, I, InternalEntries);
  };
  return getTreeRef(I.Data.loadOrGenerate(Generator));
}

Expected<CASID> InMemoryCAS::getCachedResult(CASID InputID) {
  InMemoryCacheT::pointer P = ActionCache.find(InputID.getHash());
  if (!P)
    return createResultCacheMissError(InputID);

  /// TODO: Although, consider inserting null on cache misses and returning a
  /// handle to avoid a second lookup!
  assert(P->Data && "Unexpected null in result cache");
  return CASID(P->Data->Hash);
}

Error InMemoryCAS::putCachedResult(CASID InputID, CASID OutputID) {
  const InMemoryIndexT::value_type &Expected = indexHash(OutputID.getHash());
  const InMemoryCacheT::value_type &Cached = *ActionCache.insertLazy(
      InputID.getHash(),
      [&](auto ValueConstructor) { ValueConstructor.emplace(&Expected); });

  /// TODO: Although, consider changing \a getCachedResult() to insert nullptr and
  /// returning a handle on cache misses!
  assert(Cached.Data && "Unexpected null in result cache");
  const InMemoryIndexT::value_type &Observed = *Cached.Data;
  if (&Expected == &Observed)
    return Error::success();

  return createResultCachePoisonedError(InputID, OutputID, CASID(Observed.Hash));
}

Optional<NamedTreeEntry> InMemoryCAS::lookupInTree(const TreeRef &Handle,
                                                   StringRef Name) const {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  if (Optional<InMemoryTree::NamedEntry> E = Tree.find(Name))
    return makeTreeEntry(*E);
  return None;
}

NamedTreeEntry InMemoryCAS::getInTree(const TreeRef &Handle, size_t I) const {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  assert(I < Tree.size() && "Invalid index");
  return makeTreeEntry(Tree[I]);
}

Error InMemoryCAS::forEachEntryInTree(
    const TreeRef &Handle,
    function_ref<Error(const NamedTreeEntry &)> Callback) const  {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  for (size_t I = 0, E = Tree.size(); I != E; ++I)
    if (Error E = Callback(makeTreeEntry(Tree[I])))
      return E;
  return Error::success();
}

CASID InMemoryCAS::getReferenceInNode(const NodeRef &Handle, size_t I) const {
  auto &Node = *reinterpret_cast<const InMemoryNode *>(getNodePtr(Handle));
  assert(I < Node.getRefs().size() && "Invalid index");
  return CASID(Node.getRefs()[I]->getHash());
}

Error InMemoryCAS::forEachReferenceInNode(const NodeRef &Handle,
                                          function_ref<Error(CASID)> Callback) const {
  auto &Node = *reinterpret_cast<const InMemoryNode *>(getNodePtr(Handle));
  for (const InMemoryObject *Object : Node.getRefs())
    if (Error E = Callback(CASID(Object->getHash())))
      return E;
  return Error::success();
}

std::unique_ptr<CASDB> cas::createInMemoryCAS() {
  return std::make_unique<InMemoryCAS>();
}

namespace {

/// Trie record data: 8B, atomic<uint64_t>
/// - 1-byte: StorageKind
/// - 1-byte: ObjectKind
/// - 6-bytes: DataStoreOffset (offset into referenced file)
class TrieRecord {
public:
  enum class StorageKind : uint8_t {
    /// Unknown object.
    Unknown = 0,

    /// v1.data: in the main pool, with a full DataStore record.
    Pooled = 1,

    /// v1.<TrieRecordOffset>.data: standalone, with a full DataStore record.
    Standalone = 2,

    /// v1.<TrieRecordOffset>.blob: standalone, just the data. File contents
    /// exactly the data content and file size matches the data size. No refs.
    StandaloneBlob = 3,

    /// v1.<TrieRecordOffset>.blob+0: standalone, just the data plus an
    /// extra null character ('\0'). File size is 1 bigger than the data size.
    /// No refs.
    StandaloneBlob0 = 4,
  };

  enum class ObjectKind : uint8_t {
    /// Node: refs and data.
    Node = 0,

    /// Blob: data, 8-byte alignment guaranteed, null-terminated.
    Blob = 1,

    /// Tree: custom node. Pairs of refs pointing at target (arbitrary object)
    /// and names (String), and some data to describe the kind of the entry.
    Tree = 2,

    /// String: data, no alignment guarantee, null-terminated.
    String = 3,
  };

  enum Limits : int64_t {
    // Saves files bigger than 64KB standalone instead of embedding them.
    MaxEmbeddedSize = 64LL * 1024LL - 1,
  };

  struct Data {
    StorageKind SK = StorageKind::Unknown;
    ObjectKind OK = ObjectKind::Node;
    FileOffset Offset;
  };

  static uint64_t pack(Data D) {
    assert(D.Offset.get() < (int64_t)(1ULL << 48));
    uint64_t Packed = uint64_t(D.SK) << 56 | uint64_t(D.OK) << 48 | D.Offset.get();
    assert(D.SK != StorageKind::Unknown || Packed == 0);
    return Packed;
  }

  static Data unpack(uint64_t Packed) {
    Data D;
    if (!Packed)
      return D;
    D.SK = (StorageKind)(Packed >> 56);
    D.OK = (ObjectKind)((Packed >> 48) & 0xFF);
    D.Offset = FileOffset(Packed & (-1ULL >> 48));
    return D;
  }

  TrieRecord() : Storage(0) {}

  Data load() const { return unpack(Storage); }
  bool compare_exchange_strong(Data &Existing, Data New);

private:
  std::atomic<uint64_t> Storage;
};

class InternalRef;

/// 4B reference:
/// - bits  0-29: Offset
/// - bits 30-31: Reserved for other metadata.
class InternalRef4B {
  enum : size_t {
    NumMetadataBits = 2,
    NumOffsetBits = 32 - NumMetadataBits,
  };

public:
  uint32_t getRawData() const { return Data; }
  uint64_t getOffset() const { return Data & (UINT32_MAX >> NumMetadataBits); }

private:
  friend class InternalRef;
  InternalRef4B(uint32_t Data) : Data(Data) {}
  uint32_t Data;
};

/// 8B reference:
/// - bits  0-47: Offset
/// - bits 48-63: Reserved for other metadata.
class InternalRef {
  enum : size_t {
    NumMetadataBits = 16,
    NumOffsetBits = 64 - NumMetadataBits,
  };

public:
  bool canShrinkTo4B() const {
    return getOffset() <= (UINT32_MAX >> InternalRef4B::NumMetadataBits);
  }

  /// Shrink to 4B reference.
  Optional<InternalRef4B> shrinkTo4B() const {
    if (!canShrinkTo4B())
      return None;
    uint32_t Data = getOffset();
    return InternalRef4B(Data);
  }

  uint64_t getRawData() const { return Data; }
  uint64_t getOffset() const { return Data & (UINT64_MAX >> 16); }

  InternalRef getFromRawData(uint64_t Data) {
    return InternalRef(NumMetadataBits);
  }
  InternalRef getFromOffset(uint64_t Offset) {
    assert(Offset <= (UINT64_MAX >> NumMetadataBits) && "Offset must fit in 6B");
    return InternalRef(Offset);
  }

  InternalRef(InternalRef4B SmallRef) : Data(SmallRef.Data) {}
  InternalRef &operator=(InternalRef4B SmallRef) {
    return *this = InternalRef(SmallRef);
  }

private:
  InternalRef(uint64_t Data) : Data(Data) {}
  uint64_t Data;
};

class InternalRefArrayRef {
public:
  size_t size() const { return Size; }
  bool empty() const { return !Size; }

  class iterator : public iterator_facade_base<iterator, std::random_access_iterator_tag,
                                               const InternalRef> {
  public:
    bool operator==(const iterator &RHS) const {
      if (ShiftedP != RHS.ShiftedP)
        return false;
      assert(Is4B == RHS.Is4B);
      return true;
    }
    const InternalRef &operator*() const {
      assert(ShiftedP != 0 && "Dereferencing nullptr");
      if (!Is4B)
        return *reinterpret_cast<const InternalRef *>(ShiftedP << 1);
      Ref = *reinterpret_cast<const InternalRef4B *>(ShiftedP << 1);
      return *Ref;
    }
    bool operator<(const iterator &RHS) const { return ShiftedP < RHS.ShiftedP; }
    ptrdiff_t operator-(const iterator &RHS) const {
      return (ShiftedP - RHS.ShiftedP) / getSizeFactor();
    }
    iterator &operator+=(ptrdiff_t N) {
      ShiftedP += N * getSizeFactor();
      return *this;
    }
    iterator &operator-=(ptrdiff_t N) {
      ShiftedP -= N * getSizeFactor();
      return *this;
    }

    iterator(nullptr_t = nullptr) : ShiftedP(0), Is4B(false) {}

  private:
    friend class InternalRefArrayRef;
    explicit iterator(const InternalRef *Ref)
        : ShiftedP(reinterpret_cast<uintptr_t>(Ref) >> 1), Is4B(false) {}
    explicit iterator(const InternalRef4B *Ref)
        : ShiftedP(reinterpret_cast<uintptr_t>(Ref) >> 1), Is4B(true) {}

    size_t getSizeFactor() const { return Is4B ? 2 : 4; }
    uintptr_t ShiftedP : (sizeof(uintptr_t) * 8) - 1;
    uintptr_t Is4B : 1;
    mutable Optional<InternalRef> Ref;
  };

  iterator begin() const {
    if (auto *Ref = Begin.dyn_cast<const InternalRef4B *>())
      return iterator(Ref);
    if (auto *Ref = Begin.dyn_cast<const InternalRef *>())
      return iterator(Ref);
    return iterator(nullptr);
  }
  iterator end() const { return begin() + Size; }

  /// Array accessor.
  ///
  /// Returns a reference proxy to avoid lifetime issues, since a reference
  /// derived from a InternalRef4B lives inside the iterator.
  iterator::ReferenceProxy operator[](ptrdiff_t N) const { return begin()[N]; }

  Optional<ArrayRef<InternalRef>> getAs8B() const {
    if (auto *B = Begin.dyn_cast<const InternalRef *>())
      return makeArrayRef(B, Size);
    return None;
  }

  Optional<ArrayRef<InternalRef4B>> getAs4B() const {
    if (auto *B = Begin.dyn_cast<const InternalRef4B *>())
      return makeArrayRef(B, Size);
    return None;
  }

  InternalRefArrayRef() = default;

  InternalRefArrayRef(ArrayRef<InternalRef> Refs)
      : Begin(Refs.begin()), Size(Refs.size()) {}

  InternalRefArrayRef(ArrayRef<InternalRef4B> Refs)
      : Begin(Refs.begin()), Size(Refs.size()) {}

private:
  PointerUnion<const InternalRef *, const InternalRef4B *> Begin;
  size_t Size = 0;
};

/// DataStore record data: 8B + size? + refs? + data + 0
/// - 8-bytes: Header
/// - {0,4,8}-bytes: DataSize     (may be packed in Header)
/// - {0,4,8}-bytes: NumRefs      (may be packed in Header)
/// - NumRefs*{4,8}-bytes: Refs[] (end-ptr is 8-byte aligned)
/// - <data>
/// - 1-byte: 0-term
struct DataRecordHandle {
  /// NumRefs storage: 4B, 2B, 1B, or 0B (no refs). Or, 8B, for alignment
  /// convenience to avoid computing padding later.
  enum class NumRefsFlags : uint8_t {
    Uses0B = 0U,
    Uses1B = 1U,
    Uses2B = 2U,
    Uses4B = 3U,
    Uses8B = 4U,
    Max = Uses8B,
  };

    /// DataSize storage: 8B, 4B, 2B, or 1B.
  enum class DataSizeFlags {
    Uses1B = 0U,
    Uses2B = 1U,
    Uses4B = 2U,
    Uses8B = 3U,
    Max = Uses8B,
  };

  enum class TrieOffsetFlags {
    /// TrieRecord storage: 6B or 4B.
    Uses6B = 0U,
    Uses4B = 1U,
    Max = Uses4B,
  };

  /// Kind of ref stored in Refs[]: InternalRef or InternalRef4B.
  enum class RefKindFlags {
    InternalRef = 0U,
    InternalRef4B = 1U,
    Max = InternalRef4B,
  };

  enum Counts : int {
    NumRefsShift = 0,
    NumRefsBits = 3,
    DataSizeShift = NumRefsShift + NumRefsBits,
    DataSizeBits = 2,
    TrieOffsetShift = DataSizeShift + DataSizeBits,
    TrieOffsetBits = 1,
    RefKindShift = TrieOffsetShift + TrieOffsetBits,
    RefKindBits = 1,
  };
  static_assert(((UINT32_MAX << NumRefsBits) & (uint32_t)NumRefsFlags::Max) == 0,
                "Not enough bits");
  static_assert(((UINT32_MAX << DataSizeBits) & (uint32_t)DataSizeFlags::Max) == 0,
                "Not enough bits");
  static_assert(((UINT32_MAX << TrieOffsetBits) & (uint32_t)TrieOffsetFlags::Max) == 0,
                "Not enough bits");
  static_assert(((UINT32_MAX << RefKindBits) & (uint32_t)RefKindFlags::Max) == 0,
                "Not enough bits");

  struct LayoutFlags {
    NumRefsFlags NumRefs;
    DataSizeFlags DataSize;
    TrieOffsetFlags TrieOffset;
    RefKindFlags RefKind;

    static uint64_t pack(LayoutFlags LF) {
      unsigned Packed = ((unsigned)LF.NumRefs << NumRefsShift) |
             ((unsigned)LF.DataSize << DataSizeShift) |
             ((unsigned)LF.TrieOffset << TrieOffsetShift) |
             ((unsigned)LF.RefKind << RefKindShift);
#ifndef NDEBUG
      LayoutFlags RoundTrip = unpack(Packed);
      assert(LF.NumRefs == RoundTrip.NumRefs);
      assert(LF.DataSize == RoundTrip.DataSize);
      assert(LF.TrieOffset == RoundTrip.TrieOffset);
      assert(LF.RefKind == RoundTrip.RefKind);
#endif
      return Packed;
    }
    static LayoutFlags unpack(uint64_t Storage) {
      assert(Storage <= UINT8_MAX && "Expect storage to fit in a byte");
      LayoutFlags LF;
      LF.NumRefs = (NumRefsFlags)((Storage >> NumRefsShift) & ((1U << NumRefsBits) - 1));
      LF.DataSize = (DataSizeFlags)((Storage >> DataSizeShift) & ((1U << DataSizeBits) - 1));
      LF.TrieOffset = (TrieOffsetFlags)((Storage >> TrieOffsetShift) & ((1U << TrieOffsetBits) - 1));
      LF.RefKind = (RefKindFlags)((Storage >> RefKindShift) & ((1U << RefKindBits) - 1));
      return LF;
    }
  };

  /// Header layout:
  /// - 1-byte:      LayoutFlags
  /// - 1-byte:      1B size field
  /// - {0,2}-bytes: 2B size field
  /// - {4,6}-bytes: TrieRecordOffset
  struct Header {
    uint64_t Packed;
  };

  struct Input {
    FileOffset TrieRecordOffset;
    ArrayRef<char> Data;
    ArrayRef<InternalRef> Refs;

    size_t getSizeUpperBound() const {
      return sizeof(Header) + sizeof(uint64_t) * 2 +
        + sizeof(InternalRef) * Refs.size() +
        alignTo( Data.size(),Align(8));
    }
  };

  LayoutFlags getLayoutFlags() const { return LayoutFlags::unpack(H->Packed >> 56); }
  uint64_t getTrieRecordOffset() const {
    if (getLayoutFlags().TrieOffset == TrieOffsetFlags::Uses4B)
      return H->Packed & UINT32_MAX;
    return H->Packed & (UINT64_MAX >> 16);
  }

  uint64_t getDataSize() const;
  void skipDataSize(LayoutFlags LF, int64_t &RelOffset) const;
  uint32_t getNumRefs() const;
  void skipNumRefs(LayoutFlags LF, int64_t &RelOffset) const;
  int64_t getRefsRelOffset() const;
  int64_t getDataRelOffset() const;

  static uint64_t getTotalSize(uint64_t DataRelOffset, uint64_t DataSize) {
    return DataRelOffset + DataSize + 1;
  }
  uint64_t getTotalSize() const {
    return getDataRelOffset() + getDataSize() + 1;
  }

  struct Layout {
    Layout(const Input &I);

    LayoutFlags Flags{};
    uint64_t TrieRecordOffset = 0;
    uint64_t DataSize = 0;
    uint32_t NumRefs = 0;
    int64_t RefsRelOffset = 0;
    int64_t DataRelOffset = 0;
    uint64_t getTotalSize() const {
      return DataRecordHandle::getTotalSize(DataRelOffset, DataSize);
    }
  };

  InternalRefArrayRef getRefs() const {
    assert(H && "Expected valid handle");
    auto *BeginByte = reinterpret_cast<const char *>(H) + getRefsRelOffset();
    size_t Size = getNumRefs();
    if (!Size)
      return InternalRefArrayRef();
    if (getLayoutFlags().RefKind == RefKindFlags::InternalRef4B)
      return makeArrayRef(reinterpret_cast<const InternalRef4B *>(BeginByte),
                          Size);
    return makeArrayRef(reinterpret_cast<const InternalRef *>(BeginByte), Size);
  }

  ArrayRef<char> getData() const {
    assert(H && "Expected valid handle");
    return makeArrayRef(reinterpret_cast<const char *>(H) + getDataRelOffset(),
                        getDataSize());
  }

  static DataRecordHandle create(function_ref<char *(size_t Size)> Alloc, const Input &I);
  static Expected<DataRecordHandle> createWithError(function_ref<Expected<char *>(size_t Size)> Alloc,
                                           const Input &I);
  static DataRecordHandle construct(char *Mem, const Input &I);

  static DataRecordHandle get(const char *Mem) {
    return DataRecordHandle(*reinterpret_cast<const DataRecordHandle::Header *>(Mem));
  }

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }

  DataRecordHandle() = default;
  explicit DataRecordHandle(const Header &H) : H(&H) {}

private:
  static DataRecordHandle constructImpl(char *Mem, const Input &I, const Layout &L);
  const Header *H = nullptr;
};

Expected<DataRecordHandle> DataRecordHandle::createWithError(function_ref<Expected<char *>(size_t Size)> Alloc,
                                          const Input &I) {
  Layout L(I);
  if (Expected<char *> Mem = Alloc(L.getTotalSize()))
    return constructImpl(*Mem, I, L);
  else
    return Mem.takeError();
}

DataRecordHandle DataRecordHandle::create(function_ref<char *(size_t Size)> Alloc,
                                          const Input &I) {
  Layout L(I);
  return constructImpl(Alloc(L.getTotalSize()), I, L);
}

/// On-disk CAS database and action cache (the latter should be separated).
///
/// Here's a top-level description of the current layout (could expose or make
/// this configurable in the future).
///
/// Files:
/// - db/v1.index: HashMappedTrie(name="cas.hashes[sha1]")
/// - db/v1.data: DataStore(name="cas.objects[64K]") for objects <64KB
/// - db/v1.<TrieRecordOffset>: Objects >=64KB
///
/// In theory, these could all be in one file (using a root record that points
/// at the two types of stores), but splitting the files enables setting
/// different max settings.
///
/// Eventually: update UniqueID/CASID to store:
/// - uint64_t: for BuiltinCAS, this is a pointer to Trie record
/// - CASDB*: for knowing how to compare, and for getHash()
///
/// Eventually: add ObjectHandle (update ObjectRef):
/// - uint64_t: for BuiltinCAS, this is a pointer to Data record
/// - CASDB*: for implementing APIs
///
/// Eventually: consider creating a StringPool for strings instead of using
/// RecordDataStore table.
/// - Lookup by prefix tree
/// - Store by suffix tree
class OnDiskCAS : public BuiltinCAS {
public:
  static constexpr StringLiteral IndexTableName = "llvm.cas.index[sha1]";
  static constexpr StringLiteral DataSinkTableName = "llvm.cas.data[sha1]";
  static constexpr StringLiteral ActionCacheTableName = "llvm.cas.actions[sha1->sha1]";

  static constexpr StringLiteral FilePrefix = "v1.";
  static constexpr StringLiteral FileSuffixBlob = ".blob";
  static constexpr StringLiteral FileSuffixBlob0 = ".blob0";

  class TempFile;
  class MappedTempFile;

  struct TreeObjectData;

  struct IndexProxy {
    FileOffset Offset;
    ArrayRef<uint8_t> Hash;
    TrieRecord &ObjectRef;
  };

  IndexProxy indexHash(ArrayRef<uint8_t> Hash);
  Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) final {
    return CASID(indexHash(Hash).Hash);
  }
  Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                               ArrayRef<char> Data) final;
  Expected<TreeRef> createTreeImpl(ArrayRef<uint8_t> ComputedHash, ArrayRef<NamedTreeEntry> Entries) final;
  Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                               ArrayRef<CASID> References,
                               ArrayRef<char> Data) final;

  struct BlobProxy {
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    ArrayRef<char> Data;
  };
  Expected<BlobProxy> getOrCreateBlob(IndexProxy &I, ArrayRef<char> Data);
  Expected<BlobProxy> createStandaloneBlob(IndexProxy &I, ArrayRef<char> Data);

  struct DataRecordProxy {
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    DataRecordHandle Record;
  };

  struct ObjectProxy {
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    Optional<DataRecordHandle> Record;
    Optional<ArrayRef<char>> BlobData;

    static ObjectProxy get(BlobProxy Blob) {
      return ObjectProxy{Blob.Object, Blob.Hash, None, Blob.Data};
    }
  };
  Expected<DataRecordProxy>
  getOrCreateDataRecord(IndexProxy &I, TrieRecord::ObjectKind OK,
                        DataRecordHandle::Input Input);
  Expected<Optional<DataRecordProxy>> getDataRecord(IndexProxy &I);

  Expected<MappedTempFile> createTempFile(StringRef FinalPath, uint64_t Size);
  Expected<Optional<BlobProxy>> getBlob(IndexProxy &I);
  Expected<Optional<ObjectProxy>> getObjectProxy(IndexProxy &I);
  DataRecordHandle getPooledDataRecord(FileOffset Offset) const {
    return DataRecordHandle::get(PooledData.beginData(Offset));
  }

  struct PooledDataRecord {
    FileOffset Offset;
    DataRecordHandle Record;
  };
  PooledDataRecord createPooledDataRecord(DataRecordHandle::Input Input);
  void getStandalonePath(TrieRecord::StorageKind SK, const IndexProxy &I,
                         SmallVectorImpl<char> &Path) const;

  bool isKnownObject(CASID ID) final;
  Optional<ObjectKind> getObjectKind(CASID ID) final;

  Expected<BlobRef> getBlob(CASID ID) final;
  Expected<NodeRef> getNode(CASID ID) final;
  Expected<TreeRef> getTree(CASID ID) final;

  void print(raw_ostream &OS) const final;

  Expected<CASID> getCachedResult(CASID InputID) final;
  Error putCachedResult(CASID InputID, CASID OutputID) final;

  OnDiskCAS() = delete;
  explicit OnDiskCAS(StringRef RootPath, OnDiskHashMappedTrie OnDiskObjects,
                      OnDiskHashMappedTrie OnDiskResults);

private:
  // TreeAPI.
  Optional<NamedTreeEntry> lookupInTree(const TreeRef &Tree,
                                        StringRef Name) const final;
  NamedTreeEntry getInTree(const TreeRef &Tree, size_t I) const final;
  Error forEachEntryInTree(
      const TreeRef &Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const final;

  // NodeAPI.
  CASID getReferenceInNode(const NodeRef &Ref, size_t I) const final;
  Error forEachReferenceInNode(const NodeRef &Ref,
                               function_ref<Error(CASID)> Callback) const final;


  StringRef getPathForID(StringRef BaseDir, CASID ID,
                         SmallVectorImpl<char> &Storage);

  Expected<std::unique_ptr<MemoryBuffer>> openFile(StringRef Path);
  Expected<std::unique_ptr<MemoryBuffer>> openFileWithID(StringRef BaseDir,
                                                         CASID ID);

  /// Mapping from hash to object reference.
  ///
  /// Data type is TrieRecord.
  OnDiskHashMappedTrie Index;

  /// Storage for most objects.
  ///
  /// Data type is DataRecordHandle.
  OnDiskDataAllocator PooledData;

  /// Container for "big" objects mapped in separately.
  template <size_t NumShards> class StandaloneDataMap {
    static_assert(isPowerOf2_64(NumShards), "Expected power of 2");
  public:
    MemoryBufferRef insert(ArrayRef<uint8_t> Hash,
                           std::unique_ptr<MemoryBuffer> Buffer);

    Optional<MemoryBufferRef> lookup(ArrayRef<uint8_t> Hash) const;

  private:
    struct Shard {
      DenseMap<const uint8_t *, std::unique_ptr<MemoryBuffer>> Map;
      mutable std::mutex Mutex;
    };
    Shard &getShard(ArrayRef<uint8_t> Hash) {
      return const_cast<Shard &>(
          const_cast<const StandaloneDataMap *>(this)->getShard());
    }
    const Shard &getShard(ArrayRef<uint8_t> Hash) const {
      static_assert(NumShards <= 256, "Expected only 8 bits of shard");
      return Shards[Hash[0] % NumShards];
    }

    Shard Shards[NumShards];
  };

  /// Lifetime for "big" objects not in PooledData.
  ///
  /// NOTE: Could use ThreadSafeHashMappedTrie here. For now, doing something
  /// simpler on the assumption there won't be much contention since most data
  /// is not big. If there is contention, and we've already fixed NodeRef
  /// object handles to be cheap enough to use consistently, the fix might be
  /// to use better use of them rather than optimizing this map.
  ///
  /// FIXME: Figure out the right number of shards, if any.
  StandaloneDataMap<16> StandaloneData;

  /// Action cache.
  ///
  /// FIXME: Separate out. Likely change key to be independent from CASID and
  /// stored separately.
  OnDiskHashMappedTrie Actions;

  std::string RootPath;
  std::string TempPrefix;
};

} // end anonymous namespace

/// Copy of \a sys::fs::TempFile that skips RemoveOnSignal, which is too
/// expensive to register/unregister at this rate.
///
/// FIXME: Add a TempFileManager that maintains a thread-safe list of open temp
/// files and has a signal handler registerd that removes them all.
class OnDiskCAS::TempFile {
  bool Done = false;
  TempFile(StringRef Name, int FD) : TmpName(std::string(Name)), FD(FD) {}

public:
  /// This creates a temporary file with createUniqueFile.
  static Expected<TempFile> create(const Twine &Model);
  TempFile(TempFile &&Other) { *this = std::move(Other); }
  TempFile &operator=(TempFile &&Other) {
    TmpName = std::move(Other.TmpName);
    FD = Other.FD;
    Other.Done = true;
    Other.FD = -1;
    return *this;
  }

  // Name of the temporary file.
  std::string TmpName;

  // The open file descriptor.
  int FD = -1;

  // Keep this with the given name.
  Error keep(const Twine &Name);
  Error discard();

  // This checks that keep or delete was called.
  ~TempFile() { consumeError(discard()); }
};

class OnDiskCAS::MappedTempFile {
public:
  char *data() const { return Map.data(); }
  size_t size() const { return Map.size(); }

  Error discard() {
    assert(Map && "Map already destroyed");
    Map.unmap();
    return Temp.discard();
  }

  Error keep(const Twine &Name) {
    assert(Map && "Map already destroyed");
    Map.unmap();
    return Temp.keep(Name);
  }

  MappedTempFile(TempFile Temp, sys::fs::mapped_file_region Map)
      : Temp(std::move(Temp)), Map(std::move(Map)) {}

private:
  TempFile Temp;
  sys::fs::mapped_file_region Map;
};

Error OnDiskCAS::TempFile::discard() {
  Done = true;
  if (FD != -1)
    if (std::error_code EC = sys::fs::closeFile(FD))
      return errorCodeToError(EC);
  FD = -1;

  // Always try to close and remove.
  std::error_code RemoveEC;
  if (!TmpName.empty())
    if (std::error_code EC = sys::fs::remove(TmpName))
      return errorCodeToError(EC);
  TmpName = "";

  return Error::success();
}

Error OnDiskCAS::TempFile::keep(const Twine &Name) {
  assert(!Done);
  Done = true;
  // Always try to close and rename.
  std::error_code RenameEC = sys::fs::rename(TmpName, Name);

  if (!RenameEC)
    TmpName = "";

  if (std::error_code EC = sys::fs::closeFile(FD))
    return errorCodeToError(EC);
  FD = -1;

  return errorCodeToError(RenameEC);
}

Expected<OnDiskCAS::TempFile> OnDiskCAS::TempFile::create(const Twine &Model) {
  int FD;
  SmallString<128> ResultPath;
  if (std::error_code EC = sys::fs::createUniqueFile(Model, FD, ResultPath))
    return errorCodeToError(EC);

  TempFile Ret(ResultPath, FD);
  return std::move(Ret);
}

bool TrieRecord::compare_exchange_strong(Data &Existing, Data New) {
  uint64_t ExistingPacked = pack(Existing);
  uint64_t NewPacked = pack(New);
  if (Storage.compare_exchange_strong(ExistingPacked, NewPacked))
    return true;
  Existing = unpack(ExistingPacked);
  return false;
}

DataRecordHandle DataRecordHandle::construct(char *Mem, const Input &I) {
  return constructImpl(Mem, I, Layout(I));
}

DataRecordHandle DataRecordHandle::constructImpl(char *Mem, const Input &I,
                                                 const Layout &L) {
  char *Next = Mem + sizeof(Header);

  // Fill in Packed and set other data, then come back to construct the header.
  uint64_t Packed = 0;
  Packed |= LayoutFlags::pack(L.Flags) << 56;

  // Construct DataSize.
  switch (L.Flags.DataSize) {
  case DataSizeFlags::Uses1B:
    Packed |= (uint64_t)I.Data.size() << 48;
    break;
  case DataSizeFlags::Uses2B:
    Packed |= (uint64_t)I.Data.size() << 32;
    break;
  case DataSizeFlags::Uses4B:
    assert(isAligned(Align(4), Next - Mem));
    new (Next) uint32_t(I.Data.size());
    Next += 4;
    break;
  case DataSizeFlags::Uses8B:
    assert(isAligned(Align(8), Next - Mem));
    new (Next) uint64_t(I.Data.size());
    Next += 8;
    break;
  }

  // Construct NumRefs.
  //
  // NOTE: May be writing NumRefs even if there are zero refs in order to fix
  // alignment.
  switch (L.Flags.NumRefs) {
  case NumRefsFlags::Uses0B:
    break;
  case NumRefsFlags::Uses1B:
    Packed |= (uint64_t)I.Refs.size() << 48;
    break;
  case NumRefsFlags::Uses2B:
    Packed |= (uint64_t)I.Refs.size() << 32;
    break;
  case NumRefsFlags::Uses4B:
    assert(isAligned(Align(4), Next - Mem));
    new (Next) uint32_t(I.Refs.size());
    Next += 4;
    break;
  case NumRefsFlags::Uses8B:
    assert(isAligned(Align(8), Next - Mem));
    new (Next) uint64_t(I.Refs.size());
    Next += 8;
    break;
  }

  // Construct Refs[].
  if (!I.Refs.empty()) {
    if (L.Flags.RefKind == RefKindFlags::InternalRef4B) {
      assert(isAligned(Align::Of<InternalRef4B>(), Next - Mem));
      for (auto RI = I.Refs.begin(), RE = I.Refs.end(); RI != RE; ++RI) {
        new (Next) InternalRef4B(*RI->shrinkTo4B());
        Next += sizeof(InternalRef4B);
      }
    } else {
      assert(isAligned(Align::Of<InternalRef>(), Next - Mem));
      for (auto RI = I.Refs.begin(), RE = I.Refs.end(); RI != RE; ++RI) {
        new (Next) InternalRef(*RI);
        Next += sizeof(InternalRef);
      }
    }
  }

  // Construct Data and the trailing null.
  assert(isAligned(Align(8), Next - Mem));
  llvm::copy(I.Data, Next);
  Next[I.Data.size()] = 0;

  // Construct the header itself and return.
  Header *H = new (Mem) Header{Packed};
  return DataRecordHandle(*H);
}

DataRecordHandle::Layout::Layout(const Input &I) {
  // Start initial relative offsets right after the Header.
  uint64_t RelOffset = sizeof(uint64_t);

  // Initialize the easy stuff.
  DataSize = I.Data.size();
  NumRefs = I.Refs.size();

  // Check refs size.
  Flags.RefKind = RefKindFlags::InternalRef4B;
  if (NumRefs) {
    for (InternalRef Ref : I.Refs) {
      if (!Ref.canShrinkTo4B()) {
        Flags.RefKind = RefKindFlags::InternalRef;
        break;
      }
    }
  }

  // Set the trie offset.
  TrieRecordOffset = (uint64_t)I.TrieRecordOffset.get();
  assert(TrieRecordOffset <= (UINT64_MAX >> 16));
  Flags.TrieOffset = TrieRecordOffset <= UINT32_MAX
      ? TrieOffsetFlags::Uses4B
      : TrieOffsetFlags::Uses6B;

  // Find the smallest slot available for DataSize.
  bool Has1B = true;
  bool Has2B = Flags.TrieOffset == TrieOffsetFlags::Uses4B;
  if (DataSize <= UINT8_MAX && Has1B) {
    Flags.DataSize = DataSizeFlags::Uses1B;
    Has1B = false;
  } else if (DataSize <= UINT16_MAX && Has2B) {
    Flags.DataSize = DataSizeFlags::Uses2B;
    Has2B = false;
  } else if (DataSize <= UINT32_MAX) {
    Flags.DataSize = DataSizeFlags::Uses4B;
    RelOffset += 4;
  } else {
    Flags.DataSize = DataSizeFlags::Uses8B;
    RelOffset += 8;
  }

  // Find the smallest slot available for NumRefs. Never sets NumRefs8B here.
  if (!NumRefs) {
    Flags.NumRefs = NumRefsFlags::Uses0B;
  } else if (NumRefs <= UINT8_MAX && Has1B) {
    Flags.NumRefs = NumRefsFlags::Uses1B;
    Has1B = false;
  } else if (NumRefs <= UINT16_MAX && Has2B) {
    Flags.NumRefs = NumRefsFlags::Uses2B;
    Has2B = false;
  } else {
    Flags.NumRefs = NumRefsFlags::Uses4B;
    RelOffset += 4;
  }

  // Helper to "upgrade" either DataSize or NumRefs by 4B to avoid complicated
  // padding rules when reading and writing. This also bumps RelOffset.
  //
  // The value for NumRefs is strictly limited to UINT32_MAX, but it can be
  // stored as 8B. This means we can *always* find a size to grow.
  //
  // NOTE: Only call this once.
  auto GrowSizeFieldsBy4B = [&]() {
    assert(isAligned(Align(4), RelOffset));
    RelOffset += 4;

    assert(Flags.NumRefs != NumRefsFlags::Uses8B &&
           "Expected to be able to grow NumRefs8B");

    // First try to grow DataSize. NumRefs will not (yet) be 8B, and if
    // DataSize is upgraded to 8B it'll already be aligned.
    //
    // Failing that, grow NumRefs.
    if (Flags.DataSize < DataSizeFlags::Uses4B)
      Flags.DataSize = DataSizeFlags::Uses4B; // DataSize: Packed => 4B.
    else if (Flags.DataSize < DataSizeFlags::Uses8B)
      Flags.DataSize = DataSizeFlags::Uses8B; // DataSize: 4B => 8B.
    else if (Flags.NumRefs < NumRefsFlags::Uses4B)
      Flags.NumRefs = NumRefsFlags::Uses4B; // NumRefs: Packed => 4B.
    else
      Flags.NumRefs = NumRefsFlags::Uses8B; // NumRefs: 4B => 8B.
  };

  assert(isAligned(Align(4), RelOffset));
  if (Flags.RefKind == RefKindFlags::InternalRef) {
    // List of 8B refs should be 8B-aligned. Grow one of the sizes to get this
    // without padding.
    if (!isAligned(Align(8), RelOffset))
      GrowSizeFieldsBy4B();

    assert(isAligned(Align(8), RelOffset));
    RefsRelOffset = RelOffset;
    RelOffset += 8 * NumRefs;
  } else {
    // The array of 4B refs doesn't need 8B alignment, but the data will need
    // to be 8B-aligned. Detect this now, and, if necessary, shift everything
    // by 4B by growing one of the sizes.
    uint64_t RefListSize = 4 * NumRefs;
    if (!isAligned(Align(8), RelOffset + RefListSize))
      GrowSizeFieldsBy4B();
    RefsRelOffset = RelOffset;
    RelOffset += RefListSize;
    RelOffset += 4 * NumRefs;
  }

  assert(isAligned(Align(8), RelOffset));
  DataRelOffset = RelOffset;
}

uint64_t DataRecordHandle::getDataSize() const {
  int64_t RelOffset = sizeof(Header);
  auto *DataSizePtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (getLayoutFlags().DataSize) {
  case DataSizeFlags::Uses1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case DataSizeFlags::Uses2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case DataSizeFlags::Uses4B:
    return *reinterpret_cast<const uint32_t *>(DataSizePtr);
  case DataSizeFlags::Uses8B:
    return *reinterpret_cast<const uint64_t *>(DataSizePtr);
  }
}

void DataRecordHandle::skipDataSize(LayoutFlags LF, int64_t &RelOffset) const {
  if (LF.DataSize >= DataSizeFlags::Uses4B)
    RelOffset += 4;
  if (LF.DataSize >= DataSizeFlags::Uses8B)
    RelOffset += 4;
}

uint32_t DataRecordHandle::getNumRefs() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  auto *NumRefsPtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (LF.NumRefs) {
  case NumRefsFlags::Uses0B:
    return 0;
  case NumRefsFlags::Uses1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case NumRefsFlags::Uses2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case NumRefsFlags::Uses4B:
    return *reinterpret_cast<const uint32_t *>(NumRefsPtr);
  case NumRefsFlags::Uses8B:
    return *reinterpret_cast<const uint64_t *>(NumRefsPtr);
  }
}

void DataRecordHandle::skipNumRefs(LayoutFlags LF, int64_t &RelOffset) const {
  if (LF.NumRefs >= NumRefsFlags::Uses4B)
    RelOffset += 4;
  if (LF.NumRefs >= NumRefsFlags::Uses8B)
    RelOffset += 4;
}

int64_t DataRecordHandle::getRefsRelOffset() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  skipNumRefs(LF, RelOffset);
  return RelOffset;
}

int64_t DataRecordHandle::getDataRelOffset() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  skipNumRefs(LF, RelOffset);
  uint32_t RefSize = LF.RefKind == RefKindFlags::InternalRef4B ? 4 : 8;
  RelOffset += RefSize * getNumRefs();
  return RelOffset;
}

void OnDiskCAS::print(raw_ostream &OS) const {
  OS << "on-disk-root-path: " << RootPath << "\n";
}

OnDiskCAS::IndexProxy OnDiskCAS::indexHash(ArrayRef<uint8_t> Hash) {
  auto P = Index.insertLazy(
    Hash, [](FileOffset TentativeOffset,
             OnDiskHashMappedTrie::ValueProxy TentativeValue) {
    assert(TentativeValue.Data.size() == sizeof(TrieRecord));
    assert(isAddrAligned(Align::Of<TrieRecord>(), TentativeValue.Data.data()));
    new (TentativeValue.Data.data()) TrieRecord();
    });
  assert(P && "Expected insertion");
  return IndexProxy{P.getOffset(), P->Hash,
    * reinterpret_cast<TrieRecord *>(P->Data.data())};
}

void OnDiskCAS::getStandalonePath(TrieRecord::StorageKind SK,
                                  const IndexProxy &I,
                                  SmallVectorImpl<char> &Path) const {
  StringRef Suffix;
  switch (SK) {
  default:
    assert(false && "Expected standalone storage kind");

  case TrieRecord::StorageKind::Standalone:
    Suffix = ".data";
    break;
  case TrieRecord::StorageKind::StandaloneBlob0:
    Suffix = ".blob0";
    break;
  case TrieRecord::StorageKind::StandaloneBlob:
    Suffix = ".blob";
    break;
  }

  Path.assign(RootPath.begin(), RootPath.end());
  sys::path::append(Path, FilePrefix + Twine(I.Offset.get()) + Suffix);
}

Expected<Optional<OnDiskCAS::BlobProxy>> OnDiskCAS::getBlob(IndexProxy &I) {
  Optional<OnDiskCAS::ObjectProxy> OP;
  if (Optional<Expected<NoneType>> E = moveValueInto(getObjectProxy(I), OP))
    return std::move(*E);

  assert(bool(OP->Record) != bool(OP->BlobData));
  if (OP->BlobData)
    return BlobProxy{OP->Object, OP->Hash, *OP->BlobData};

  // Blobs should not have references.
  if (!OP->Record->getRefs().empty())
    return createCorruptObjectError(CASID(I.Hash));
  return BlobProxy{OP->Object, OP->Hash, OP->Record->getData()};
}

Expected<Optional<OnDiskCAS::DataRecordProxy>> OnDiskCAS::getDataRecord(IndexProxy &I) {
  Optional<OnDiskCAS::ObjectProxy> OP;
  if (Optional<Expected<NoneType>> E = moveValueInto(getObjectProxy(I), OP))
    return std::move(*E);

  assert(OP->Record && "Expected record");
  assert(!OP->BlobData && "Unexpected blob");
  return DataRecordProxy{OP->Object, OP->Hash, *OP->Record};
}

Expected<Optional<OnDiskCAS::ObjectProxy>> OnDiskCAS::getObjectProxy(IndexProxy &I) {
  TrieRecord::Data Object = I.ObjectRef.load();
  if (Object.SK == TrieRecord::StorageKind::Unknown)
    return None;

  bool Blob0 = false;
  bool Blob = false;
  switch (Object.SK) {
  default:
    return createCorruptObjectError(CASID(I.Hash));

  case TrieRecord::StorageKind::Pooled: {
    DataRecordHandle Handle = DataRecordHandle::get(PooledData.beginData(Object.Offset));
    return ObjectProxy{Object, I.Hash, Handle, None};
  }

  case TrieRecord::StorageKind::Standalone:
    break;
  case TrieRecord::StorageKind::StandaloneBlob0:
    Blob = Blob0 = true;
    break;
  case TrieRecord::StorageKind::StandaloneBlob:
    Blob = true;
    break;
  }

  assert(!I.Offset && "Unexpected offset for standalone objects");

  // Helper for creating the return.
  auto createProxy = [&](MemoryBufferRef Buffer) -> ObjectProxy {
    if (Blob)
      return ObjectProxy{Object, I.Hash, None, toArrayRef(Buffer.getBuffer().drop_back(Blob0))};
    return ObjectProxy{Object, I.Hash, DataRecordHandle::get(Buffer.getBuffer().data()), None};
  };

  // Check if we've loaded it already.
  if (Optional<MemoryBufferRef> Buffer = StandaloneData.lookup(I.Hash))
    return createProxy(*Buffer);

  // Load it from disk.
  SmallString<256> Path;
  getStandalonePath(Object.SK, I, Path);
  ErrorOr<std::unique_ptr<MemoryBuffer>> OwnedBuffer =
      MemoryBuffer::getFile(Path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!OwnedBuffer)
    return createCorruptObjectError(CASID(I.Hash));

  MemoryBufferRef Buffer = StandaloneData.insert(I.Hash, std::move(*OwnedBuffer));
  return createProxy(Buffer);
}

Expected<OnDiskCAS::MappedTempFile>
OnDiskCAS::createTempFile(StringRef FinalPath, uint64_t Size) {
  assert(Size && "Unexpected request for an empty temp file");
  Expected<TempFile> File = TempFile::create(FinalPath + ".%%%%%%");
  if (!File)
    return File.takeError();

  if (auto EC =
          sys::fs::resize_file_before_mapping_readwrite(File->FD, Size))
    return createFileError(File->TmpName, EC);

  std::error_code EC;
  sys::fs::mapped_file_region Map(
      sys::fs::convertFDToNativeFile(File->FD),
      sys::fs::mapped_file_region::readwrite, Size, 0, EC);
  if (EC)
    return createFileError(File->TmpName, EC);
  return MappedTempFile(std::move(*File), std::move(Map));
}

Expected<OnDiskCAS::BlobProxy> OnDiskCAS::createStandaloneBlob(IndexProxy &I,
                                                               ArrayRef<char> Data) {
  assert(Data.size() > TrieRecord::MaxEmbeddedSize &&
         "Expected a bigger file for external content...");

  bool Blob0 = isAligned(Align(getPageSize()), Data.size());
  TrieRecord::StorageKind SK =
      Blob0 ? TrieRecord::StorageKind::StandaloneBlob0
              : TrieRecord::StorageKind::StandaloneBlob;

  SmallString<256> Path;
  int64_t FileSize = Data.size() + Blob0;
  getStandalonePath(SK, I, Path);

  // write the file.
  Expected<MappedTempFile> File = createTempFile(Path, FileSize);
  llvm::copy(Data, File->data());
  if (Blob0)
    File->data()[Data.size()] = 0;
  if (Error E = File->keep(Path))
    return std::move(E);

  // Store the object reference. If there was a race, getBlob() will have the
  // new value.
  TrieRecord::Data Existing;
  if (!I.ObjectRef.compare_exchange_strong(
          Existing, TrieRecord::Data{SK, TrieRecord::ObjectKind::Blob, FileOffset()})) {
    // If there was a race, confirm that the new value has valid storage.
    if (Existing.SK == TrieRecord::StorageKind::Unknown)
      return createCorruptObjectError(CASID(I.Hash));

    // Fall through...
  }

  // Get and return the inserted blob.
  return dereferenceValue(getBlob(I),
                    [&]() { return createCorruptObjectError(CASID(I.Hash)); });
}

Expected<OnDiskCAS::DataRecordProxy>
OnDiskCAS::getOrCreateDataRecord(IndexProxy &I, TrieRecord::ObjectKind OK,
                                 DataRecordHandle::Input Input) {
  assert(OK != TrieRecord::ObjectKind::Blob &&
         "Expected blobs to be handled elsewhere");

  // See if it already exists.
  if (Optional<Expected<DataRecordProxy>> Record =
         dereferenceValue(getDataRecord(I)))
    return std::move(*Record);

  // Compute the storage kind, allocate it, and create the record.
  TrieRecord::StorageKind SK = TrieRecord::StorageKind::Unknown;
  FileOffset PoolOffset;
  SmallString<256> Path;
  Optional<MappedTempFile> File;
  auto Alloc = [&](size_t Size) -> Expected<char *> {
    if (Size <= TrieRecord::MaxEmbeddedSize) {
      SK = TrieRecord::StorageKind::Pooled;
      OnDiskDataAllocator::pointer P = PooledData.allocate(Size);
      PoolOffset = P.getOffset();
      return P->data();
    }

    SK = TrieRecord::StorageKind::Standalone;
    getStandalonePath(SK, I, Path);
    if (Error E = createTempFile(Path, Size).moveInto(File))
      return std::move(E);
    return File->data();
  };
  DataRecordHandle Record;
  if (Error E = DataRecordHandle::createWithError(Alloc, Input).moveInto(Record))
    return std::move(E);
  assert(SK != TrieRecord::StorageKind::Unknown);
  assert(bool(File) != bool(PoolOffset) &&
         "Expected either a mapped file or a pooled offset");

  // Check for someone else writing in the meantime.
  TrieRecord::Data NewObject{SK, OK, PoolOffset};
  TrieRecord::Data Existing = I.ObjectRef.load();

  // Decide what to do with the file. Better to discard than overwrite if
  // another thread/process has already added this.
  if (File) {
    if (Existing.SK == TrieRecord::StorageKind::Unknown) {
      // Keep the file!
      if (Error E = File->keep(Path))
        return std::move(E);
    } else {
      File.reset();
    }
  }

  // If there wasn't a race (yet), try storing our object.
  if (Existing.SK == TrieRecord::StorageKind::Unknown)
    (void)I.ObjectRef.compare_exchange_strong(
           Existing, NewObject);

  // If there was a race, confirm that the new value has valid storage.
  //
  // TODO: Find a way to reuse the storage from the new-but-abandoned record
  // handle.
  if (Existing.SK == TrieRecord::StorageKind::Unknown)
    return createCorruptObjectError(CASID(I.Hash));

  // Get and return the record.
  return dereferenceValue(getDataRecord(I),
                    [&]() { return createCorruptObjectError(CASID(I.Hash)); });
}

Expected<OnDiskCAS::BlobProxy> OnDiskCAS::getOrCreateBlob(IndexProxy &I, ArrayRef<char> Data) {
  // See if it already exists.
  if (Optional<Expected<BlobProxy>> Blob = dereferenceValue(getBlob(I)))
    return std::move(*Blob);

  if (Data.size() > TrieRecord::MaxEmbeddedSize)
    return createStandaloneBlob(I, Data);

  PooledDataRecord PDR = createPooledDataRecord(
      DataRecordHandle::Input{I.Offset, Data, None});

  TrieRecord::Data Blob;
  Blob.OK = TrieRecord::ObjectKind::Blob;
  Blob.SK = TrieRecord::StorageKind::Pooled;
  Blob.Offset = PDR.Offset;
  return BlobProxy{Blob, I.Hash, PDR.Record.getData()};
}

OnDiskCAS::PooledDataRecord OnDiskCAS::createPooledDataRecord(DataRecordHandle::Input Input) {
  FileOffset Offset;
  auto Alloc = [&](size_t Size) -> char* {
    OnDiskDataAllocator::pointer P = PooledData.allocate(Size);
    Offset = P.getOffset();
    return P->data();
  };
  DataRecordHandle Record = DataRecordHandle::create(Alloc, Input);
  assert(Offset && "Should always have an offset");
  return PooledDataRecord{Offset, Record};
}

Expected<BlobRef> OnDiskCAS::createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                            ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  IndexProxy I = indexHash(ComputedHash);

  BlobProxy Blob;
  if (Error E = getOrCreateBlob(I, Data).moveInto(Blob))
    return std::move(E);

  return makeBlobRef(CASID(Blob.Hash), Blob.Data);
}

Expected<TreeRef> OnDiskCAS::createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                                            ArrayRef<NamedTreeEntry> Entries) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  IndexProxy I = indexHash(ComputedHash);

  DataRecordProxy;
  if (const InMemoryObject *Tree = I.Data.load())
    return getTreeRef(*Tree);

  SmallVector<InternalRef> Refs;
  SmallVector<char> Data;
  for (const NamedTreeEntry &E : SortedEntries) {
    Optional<InternalRef> Ref = getObjectRef(E.getID());
    if (!Ref)
      return createUnknownObjectError(E.getID());

    Refs.push_back(Ref);
    Refs.push_back(getOrCreateStringRef(E.getName()));
    Data.push_back((uint8_t)getStableKind(E.getKind()));
  }

  // Create the object.
  DataRecordProxy Tree;
  if (Error E = getOrCreateDataRecord(
      I, TrieRecord::ObjectKind::Tree,
      TrieRecord::Input{I.Offset, Refs, Data}).moveInto(Tree))
    return std::move(E);
  return makeTreeRef(CASID(Tree.Hash), &Tree.Record.getHeader(), Refs.size());
}

Expected<NodeRef> OnDiskCAS::createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                            ArrayRef<CASID> References,
                                            ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  IndexProxy I = indexHash(ComputedHash);

  DataRecordProxy;
  if (const InMemoryObject *Node = I.Data.load())
    return getNodeRef(*Node);

  SmallVector<InternalRef> Refs;
  for (const CASID &ID : SortedEntries) {
    Optional<InternalRef> Ref = getObjectRef(ID.getID());
    if (!Ref)
      return createUnknownObjectError(ID.getID());

    Refs.push_back(Ref);
  }

  // Create the object.
  DataRecordProxy Node;
  if (Error E = getOrCreateDataRecord(
      I, TrieRecord::ObjectKind::Node,
      TrieRecord::Input{I.Offset, Refs, Data}).moveInto(Node))
    return std::move(E);
  return makeNodeRef(CASID(Node.Hash), &Node.Record.getHeader(), Refs.size(),
                     Node.Record.getData());
}


using TreeObjectData = BuiltinCAS::TreeObjectData;
using ObjectContentReference = BuiltinCAS::ObjectContentReference;
using ObjectAndID = BuiltinCAS::ObjectAndID;
using ContentReference = BuiltinCAS::ContentReference;
using AbstractReferenceList = BuiltinCAS::AbstractReferenceList;
using AbstractObjectReference = BuiltinCAS::AbstractObjectReference;

// Stable numbers for serializing trees.
constexpr static uint32_t EntryKindIDForTree = 1;
constexpr static uint32_t EntryKindIDForRegular = 2;
constexpr static uint32_t EntryKindIDForExecutable = 3;
constexpr static uint32_t EntryKindIDForSymlink = 4;

constexpr static StringLiteral CASTreeMagic = "\xcas-tree\001";
static_assert(CASTreeMagic.size() % alignof(uint64_t) == 0,
              "Magic not 8-byte aligned");

static void printTree(raw_ostream &OS, ArrayRef<NamedTreeEntry> SortedEntries,
                      SmallVectorImpl<CASID> &References) {
  // Hash input has format:
  //
  //  input := header unnamed-entry* name*
  //  header := 'cas-tree' num-entries
  //  unnamed-entry := kind name-offset
  //  name := name-string NULL
  OS << CASTreeMagic;
  support::endian::Writer EW(OS, support::endianness::little);
  uint64_t StringsOffset = 0;
  for (const NamedTreeEntry &Entry : SortedEntries) {
    assert(Entry.getID().getHash().size() == sizeof(HashType));
    References.push_back(Entry.getID());

    uint64_t Kind = 0;
    switch (Entry.getKind()) {
    case TreeEntry::Regular:
      Kind = EntryKindIDForRegular;
      break;
    case TreeEntry::Tree:
      Kind = EntryKindIDForTree;
      break;
    case TreeEntry::Executable:
      Kind = EntryKindIDForExecutable;
      break;
    case TreeEntry::Symlink:
      Kind = EntryKindIDForSymlink;
      break;
    }
    assert(Kind < (1u << 8) && "Kind needs to be 8 bits");

    assert(StringsOffset < (1ull << 56) && "Absurdly large string?");
    EW.write(StringsOffset | (Kind << 56));
    StringsOffset +=
        alignTo(Entry.getName().size() + 1, Align(4)) + sizeof(uint32_t);
  }
  for (const NamedTreeEntry &Entry : SortedEntries) {
    EW.write(uint32_t(Entry.getName().size()));
    OS << Entry.getName();
    for (int I = 0, E = alignTo(Entry.getName().size() + 1, Align(4)) -
                        Entry.getName().size();
         I != E; ++I)
      OS.write(0);
  }
}

CASID BuiltinCAS::ObjectContentReference::getReference(size_t I) const {
  assert(I < getNumReferences() && "Reference out of range");
  return CASID(
      bufferAsRawHash(ReferenceBlock.substr(I * sizeof(HashType), sizeof(HashType))));
}

struct BuiltinCAS::TreeObjectData {
  Optional<ArrayRef<CASID>> References;
  Optional<StringRef> Data;

  Optional<SmallVector<CASID, 16>> ReferenceStorage;
  Optional<SmallString<1024>> DataStorage;
};

BuiltinCAS::AbstractObjectReference::AbstractObjectReference(
    const TreeObjectData &Object)
    : References(*Object.References), Data(*Object.Data) {}

static bool isValidTree(ObjectContentReference Object) {
  // Check for magic.
  StringRef Data = Object.Data;
  if (!Data.consume_front(CASTreeMagic))
    return false;

  // Check for an empty tree.
  uint64_t NumEntries = Object.getNumReferences();
  if (!NumEntries)
    return Data.empty();

  // Check that there's space for the strings table.
  //
  // TODO: Sanity check a couple of tree entries.
  const size_t EntrySize = sizeof(uint64_t);
  return NumEntries * EntrySize < Data.size();
}

static Optional<NamedTreeEntry> getTreeEntryImpl(ObjectContentReference Object,
                                                 size_t I) {
  uint64_t NumEntries = Object.getNumReferences();
  assert(I < NumEntries);
  CASID ID = Object.getReference(I);

  assert(Object.Data.size() >= CASTreeMagic.size() && "Expected valid tree");
  const char *Start = Object.Data.begin() + CASTreeMagic.size();

  const size_t EntrySize = sizeof(uint64_t);
  size_t StringsTableOffset = NumEntries * EntrySize;
  assert(ptrdiff_t(StringsTableOffset) < Object.Data.end() - Start &&
         "Expected valid tree");
  const char *StringsTable = Start + StringsTableOffset;

  using namespace llvm::support;
  uint64_t EntryData = endian::read<uint64_t, endianness::little, aligned>(
      Start + I * EntrySize);
  uint64_t KindID = EntryData >> 56;
  uint64_t StringOffset = EntryData & ((1ull << 56) - 1ull);

  // Get the kind.
  Optional<TreeEntry::EntryKind> Kind;
  switch (KindID) {
  default:
    return None;
  case EntryKindIDForRegular:
    Kind = TreeEntry::Regular;
    break;
  case EntryKindIDForTree:
    Kind = TreeEntry::Tree;
    break;
  case EntryKindIDForExecutable:
    Kind = TreeEntry::Executable;
    break;
  case EntryKindIDForSymlink:
    Kind = TreeEntry::Symlink;
    break;
  }

  // Get the name.
  if (ptrdiff_t(StringOffset + sizeof(uint32_t)) >
      Object.Data.end() - StringsTable)
    return None;
  const char *String = StringsTable + StringOffset;
  uint32_t StringSize =
      endian::readNext<uint32_t, endianness::little, aligned>(String);
  if (ptrdiff_t(StringSize) + 1 > Object.Data.end() - String)
    return None;
  if (String[StringSize] != 0)
    return None;
  StringRef Name(String, StringSize);

  return NamedTreeEntry(ID, *Kind, Name);
}

static NamedTreeEntry getTreeEntry(ObjectContentReference Object, size_t I) {
  Optional<NamedTreeEntry> MaybeEntry = getTreeEntryImpl(Object, I);

  // FIXME: probably want to a report a corrupt object?
  assert(MaybeEntry && "Parse error");
  return *MaybeEntry;
}

static TreeObjectData makeTree(ArrayRef<NamedTreeEntry> SortedEntries) {
  TreeObjectData Tree;
  {
    // Reserve enough references.
    Tree.ReferenceStorage.emplace();
    Tree.ReferenceStorage->reserve(SortedEntries.size());

    // Reserve enough chars.
    size_t NumChars = CASTreeMagic.size();
    for (auto &E : SortedEntries)
      NumChars += sizeof(uint64_t) + sizeof(uint32_t) +
                  alignTo(E.getName().size() + sizeof(char), Align(4));
    Tree.DataStorage.emplace();
    Tree.DataStorage->reserve(NumChars);

    // Print the tree.
    raw_svector_ostream OS(*Tree.DataStorage);
    printTree(OS, SortedEntries, *Tree.ReferenceStorage);
    assert(Tree.DataStorage->size() == NumChars && "Reservation doesn't match");
  }
  assert(Tree.ReferenceStorage->size() == SortedEntries.size() && "Reservation doesn't match");

  Tree.Data = *Tree.DataStorage;
  Tree.References = *Tree.ReferenceStorage;
  return Tree;
}

HashType AbstractObjectReference::computeHash() const {
  SmallString<sizeof(uint64_t) * 3> Sizes;
  {
    raw_svector_ostream OS(Sizes);
    llvm::support::endian::Writer EW(OS, support::endianness::little);
    EW.write(uint64_t(References.getNumBytes()));
    EW.write(uint64_t(Data.size()));
  }

  SHA1 Hasher;
  Hasher.update(Sizes);
  References.updateHash(Hasher);
  Hasher.update(Data);
  return makeHash(bufferAsRawHash(Hasher.final()));
}

size_t BuiltinCAS::AbstractReferenceList::getNumBytes() const {
  return ReferenceBlock ? ReferenceBlock->size()
                        : ReferenceList->size() * sizeof(HashType);
}

size_t BuiltinCAS::AbstractReferenceList::getNumReferences() const {
  return ReferenceBlock ? ReferenceBlock->size() / sizeof(HashType)
                        : ReferenceList->size();
}

void BuiltinCAS::AbstractReferenceList::appendTo(
    SmallVectorImpl<char> &Sink) const {
  if (ReferenceBlock) {
    Sink.append(ReferenceBlock->begin(), ReferenceBlock->end());
    return;
  }
  for (CASID ID : *ReferenceList) {
    auto Hash = rawHashAsBuffer(ID.getHash());
    assert(Hash.size() == sizeof(HashType));
    Sink.append(Hash.begin(), Hash.end());
  }
}

void BuiltinCAS::AbstractReferenceList::updateHash(SHA1 &Hasher) const {
  if (ReferenceBlock) {
    Hasher.update(*ReferenceBlock);
    return;
  }
  for (CASID ID : *ReferenceList) {
    assert(ID.getHash().size() == sizeof(HashType));
    Hasher.update(ID.getHash());
  }
}

StringRef BuiltinCAS::AbstractObjectReference::build(
    SmallVectorImpl<char> &Metadata, SmallVectorImpl<char> &DataStorage) const {
  assert(Metadata.empty());
  assert(DataStorage.empty());

  Metadata.reserve(sizeof(uint64_t) * 2);
  raw_svector_ostream OS(Metadata);
  support::endian::Writer EW(OS, support::endianness::little);
  EW.write(uint64_t(References.getNumReferences()));
  EW.write(uint64_t(Data.size()));

  // No references. No need to copy out data.
  if (!References.getNumReferences())
    return Data;

  // Reserve enough space for the reference block, padding for 8 byte
  // alignment, and the data.
  uint64_t ReferenceBlockSize = References.getNumBytes();
  uint64_t StoragePadding =
      alignTo(ReferenceBlockSize, Align(8)) - ReferenceBlockSize;
  assert(StoragePadding < 8);
  uint64_t ExpectedStorageSize =
      ReferenceBlockSize + StoragePadding + Data.size();
  DataStorage.reserve(ExpectedStorageSize);

  // Build the references and data.
  References.appendTo(DataStorage);
  assert(DataStorage.size() == ReferenceBlockSize);
  if (StoragePadding)
    DataStorage.resize(DataStorage.size() + StoragePadding, 0);
  DataStorage.append(Data.begin(), Data.end());
  assert(ExpectedStorageSize == DataStorage.size());
  return StringRef(DataStorage.begin(), DataStorage.size());
}

Optional<ObjectContentReference>
ObjectContentReference::getFromContent(ContentReference Content) {
  if (Content.Metadata.size() < sizeof(uint64_t) * 2)
    return None;

  using namespace llvm::support;

  // Read the metadata.
  const char *Current = Content.Metadata.begin();
  size_t NumReferences =
      endian::readNext<uint64_t, endianness::little, aligned>(Current);
  size_t DataSize =
      endian::readNext<uint64_t, endianness::little, aligned>(Current);
  ObjectContentReference Object;

  // Read the reference block and data.
  uint64_t ReferenceBlockSize = NumReferences * sizeof(HashType);
  uint64_t StoragePadding =
      alignTo(ReferenceBlockSize, Align(8)) - ReferenceBlockSize;
  if (Content.Data.size() != ReferenceBlockSize + StoragePadding + DataSize)
    return None; // Error.
  Object.ReferenceBlock = Content.Data.take_front(ReferenceBlockSize);
  Object.Data = Content.Data.take_back(DataSize);
  return Object;
}

bool BuiltinCAS::isKnownObject(CASID ID) {
  return !errorToBool(getObject(ID).takeError());
}

Optional<ObjectKind> BuiltinCAS::getObjectKind(CASID ID) {
  Expected<ObjectAndID> ExpectedObject = getObject(ID);
  if (!ExpectedObject) {
    consumeError(ExpectedObject.takeError());
    return None;
  }
  if (!errorToBool(getTreeFromObject(*ExpectedObject).takeError()))
    return ObjectKind::Tree;
  if (!errorToBool(getBlobFromObject(*ExpectedObject).takeError()))
    return ObjectKind::Blob;
  return ObjectKind::Node;
}

Expected<ObjectAndID> BuiltinCAS::getObject(CASID ID) {
  ArrayRef<uint8_t> Hash = ID.getHash();
  auto Lookup = ObjectCache.find(Hash);
  if (Lookup)
    return ObjectAndID{Lookup->Data, CASID(Lookup->Hash)};

  if (isInMemoryOnly())
    return createUnknownObjectError(ID);

  Optional<MappedContentReference> File = openOnDisk(*OnDiskObjects, Hash);
  if (!File)
    return createUnknownObjectError(ID);

  ContentReference Content = persistMappedContentInMemory(std::move(*File));
  Optional<ObjectContentReference> Object =
      ObjectContentReference::getFromContent(Content);
  if (!Object)
    return createCorruptObjectError(ID);

  auto Insertion = ObjectCache.insert(
      Lookup, ObjectCacheType::value_type(makeHash(Hash), *Object));
  return ObjectAndID{Insertion->Data, CASID(Insertion->Hash)};
}

Expected<BlobRef> BuiltinCAS::getBlobFromObject(Expected<ObjectAndID> Object) {
  if (!Object)
    return Object.takeError();

  if (Object->Object.getNumReferences())
    return createInvalidObjectError(Object->ID, "blob");

  return makeBlobRef(Object->ID, Object->Object.Data);
}

Expected<TreeRef> BuiltinCAS::getTreeFromObject(Expected<ObjectAndID> Object) {
  if (!Object)
    return Object.takeError();

  if (!isValidTree(Object->Object))
    return createInvalidObjectError(Object->ID, "tree");

  return makeTreeRef(Object->ID, &Object->Object,
                     Object->Object.getNumReferences());
}

Optional<NamedTreeEntry> BuiltinCAS::lookupInTree(const TreeRef &Tree,
                                                  StringRef Name) const {
  // FIXME: Consider adding an on-disk hash table for large trees.
  auto &Object = *static_cast<const ObjectContentReference *>(getTreePtr(Tree));

  // Start with a binary search, if there are enough entries.
  const size_t MaxLinearSearchSize = 4;
  size_t First = 0, Last = Object.getNumReferences();
  while (Last - First > MaxLinearSearchSize) {
    size_t I = (First + Last) / 2;
    NamedTreeEntry Entry = getTreeEntry(Object, I);
    switch (Name.compare(Entry.getName())) {
    case 0:
      return Entry;
    case -1:
      Last = I;
      break;
    case 1:
      First = I + 1;
      break;
    }
  }

  // Use a linear search for small trees.
  for (; First != Last; ++First) {
    NamedTreeEntry Entry = getTreeEntry(Object, First);
    if (Name == Entry.getName())
      return Entry;
  }

  return None;
}

NamedTreeEntry BuiltinCAS::getInTree(const TreeRef &Tree, size_t I) const {
  auto &Object = *static_cast<const ObjectContentReference *>(getTreePtr(Tree));
  return getTreeEntry(Object, I);
}

Error BuiltinCAS::forEachEntryInTree(
    const TreeRef &Tree,
    function_ref<Error(const NamedTreeEntry &)> Callback) const {
  auto &Object = *static_cast<const ObjectContentReference *>(getTreePtr(Tree));
  for (size_t I = 0, E = Object.getNumReferences(); I != E; ++I)
    if (Error E = Callback(getTreeEntry(Object, I)))
      return E;
  return Error::success();
}

Expected<TreeRef> BuiltinCAS::createTree(ArrayRef<NamedTreeEntry> Entries) {
  SmallVector<NamedTreeEntry> Sorted(Entries.begin(), Entries.end());

  // Ensure a stable order for tree entries and ignore name collisions.
  std::stable_sort(Sorted.begin(), Sorted.end());
  Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());

  // Check each name is valid.
  //
  // FIXME: Is this too expensive? Maybe we don't really care...
  for (const NamedTreeEntry &Entry : Sorted)
    if (Entry.getName().find_first_of("/\0\n") != StringRef::npos)
      return errorCodeToError(
          std::make_error_code(std::errc::invalid_argument));

  return getTreeFromObject(createObject(makeTree(Sorted)));
}

Expected<NodeRef> BuiltinCAS::getNodeFromObject(Expected<ObjectAndID> Object) {
  if (!Object)
    return Object.takeError();

  return makeNodeRef(Object->ID, &Object->Object,
                     Object->Object.getNumReferences(), Object->Object.Data);
}

CASID BuiltinCAS::getReferenceInNode(const NodeRef &Ref, size_t I) const {
  auto &Object = *static_cast<const ObjectContentReference *>(getNodePtr(Ref));
  return Object.getReference(I);
}

Error BuiltinCAS::forEachReferenceInNode(
    const NodeRef &Ref, function_ref<Error(CASID)> Callback) const {
  auto &Object = *static_cast<const ObjectContentReference *>(getNodePtr(Ref));
  for (size_t I = 0, E = Object.getNumReferences(); I != E; ++I)
    if (Error E = Callback(Object.getReference(I)))
      return E;
  return Error::success();
}

Expected<ObjectAndID> BuiltinCAS::createObject(
    AbstractObjectReference Object,
    Optional<sys::fs::mapped_file_region> NullTerminatedMap) {
  HashType Hash = Object.computeHash();
  auto Lookup = ObjectCache.find(Hash);
  if (Lookup) {
    assert(Object.Data == Lookup->Data.Data);
    return ObjectAndID{Lookup->Data, CASID(Lookup->Hash)};
  }

  SmallString<256> Metadata;
  SmallString<1024> DataStorage;

  // Build the metadata and data. We can reuse the null-terminated map only if
  // the data wasn't moved to DataStorage.
  StringRef ReferenceBlockAndData = Object.build(Metadata, DataStorage);
  Optional<sys::fs::mapped_file_region> DataMapToPersist;
  if (DataStorage.empty())
    DataMapToPersist = std::move(NullTerminatedMap);

  ContentReference Content = getOrCreatePersistentContent(
      Hash, Metadata, ReferenceBlockAndData, std::move(DataMapToPersist));
  Optional<ObjectContentReference> PersistentObject =
      ObjectContentReference::getFromContent(Content);
  assert(PersistentObject && "Expected persistant object to be valid");

  auto Insertion = ObjectCache.insert(
      Lookup, ObjectCacheType::value_type(Hash, *PersistentObject));
  return ObjectAndID{Insertion->Data, CASID(Insertion->Hash)};
}

Expected<BlobRef> BuiltinCAS::createBlobImpl(
    StringRef Data, Optional<sys::fs::mapped_file_region> NullTerminatedMap) {
  return getBlobFromObject(
      createObject(AbstractObjectReference(None, Data),
                   std::move(NullTerminatedMap)));
}

StringRef BuiltinCAS::persistBufferInMemory(
    StringRef Buffer, bool ShouldNullTerminate,
    Optional<sys::fs::mapped_file_region> NullTerminatedMap) {
  if (NullTerminatedMap) {
    assert(NullTerminatedMap->data() == Buffer.begin());
    assert(Buffer.end()[0] == 0);
    new (PersistentMaps.Allocate())
        sys::fs::mapped_file_region(std::move(*NullTerminatedMap));
    return Buffer;
  }

  auto GetPadding = [](uint64_t Size) -> uint64_t {
    if (uint64_t Remainder = Size % 8)
      return 8 - Remainder;
    return 0;
  };
  const uint64_t PaddingSize = ShouldNullTerminate
                                   ? (GetPadding(Buffer.size() + 1) + 1)
                                   : GetPadding(Buffer.size());

  // Use the in-memory allocator for content. This is only expected when we
  // don't have on-disk objects.
  assert(isInMemoryOnly());
  char *InMemoryBuffer =
      AlignedInMemoryStrings->Allocate(Buffer.size() + PaddingSize);
  ::memcpy(InMemoryBuffer, Buffer.begin(), Buffer.size());
  ::memset(InMemoryBuffer + Buffer.size(), 0, PaddingSize);
  return StringRef(InMemoryBuffer, Buffer.size());
}

Expected<CASID> BuiltinCAS::getCachedResult(CASID InputID) {
  if (!isOnDisk()) {
    // Look up the result in-memory (only if it's not on-disk).
    auto Lookup = ResultCache.find(InputID.getHash());
    if (Lookup)
      return CASID(Lookup->Data);

    // FIXME: Odd to have an error for a cache miss. This function should
    // have a different API.
    return createResultCacheMissError(InputID);
  }

  // Look up the result on-disk.
  Optional<MappedContentReference> File =
      openOnDisk(*OnDiskResults, InputID.getHash());
  if (!File)
    return createResultCacheMissError(InputID);
  if (File->Metadata != "results") // FIXME: Should cache this object.
    return createCorruptObjectError(InputID);

  // Expect all cached CASIDs to be directly available, without needing an
  // additional mapped file.
  assert(!File->Map && "Unexpected mapped_file_region in result cache");
  if (File->Data.size() != sizeof(HashType))
    reportAsFatalIfError(createResultCacheCorruptError(InputID));
  return CASID(bufferAsRawHash(File->Data));
}

Error BuiltinCAS::putCachedResult(CASID InputID, CASID OutputID) {
  // Helper for confirming cached results are consistent.
  auto checkOutput = [&](ArrayRef<uint8_t> ExistingOutput) -> Error {
    if (ExistingOutput == OutputID.getHash())
      return Error::success();
    // FIXME: probably...
    reportAsFatalIfError(createResultCachePoisonedError(InputID, OutputID,
                                                        CASID(ExistingOutput)));
    return Error::success();
  };

  ArrayRef<uint8_t> InputHash = InputID.getHash();
  if (!isOnDisk()) {
    // Store in-memory and check against existing results.
    return checkOutput(
        ResultCache
            .insert(ResultCacheType::pointer{},
                    ResultCacheType::value_type(InputHash, makeHash(OutputID)))
            ->Data);
  }

  // Store on-disk and check against existing results.
  ArrayRef<uint8_t> OutputHash = OutputID.getHash();
  MappedContentReference File =
      OnDiskResults->insert(InputHash, "results", rawHashAsBuffer(OutputHash));
  assert(!File.Map && "Unexpected mapped_file_region in result cache");
  return checkOutput(bufferAsRawHash(File.Data));
}

Optional<BuiltinCAS::MappedContentReference>
BuiltinCAS::openOnDisk(OnDiskHashMappedTrie &Trie, ArrayRef<uint8_t> Hash) {
  if (auto Lookup = Trie.lookup(Hash))
    return Lookup.take();
  return None;
}

Expected<std::unique_ptr<MemoryBuffer>> BuiltinCAS::openFile(StringRef Path) {
  assert(!isInMemoryOnly());
  return errorOrToExpected(llvm::MemoryBuffer::getFile(Path));
}

Expected<std::unique_ptr<MemoryBuffer>>
BuiltinCAS::openFileWithID(StringRef BaseDir, CASID ID) {
  assert(!isInMemoryOnly());
  SmallString<256> Path = StringRef(*RootPath);
  sys::path::append(Path, BaseDir);

  SmallString<64> PrintableHash;
  extractPrintableHash(ID, PrintableHash);
  sys::path::append(Path, PrintableHash);

  Expected<std::unique_ptr<MemoryBuffer>> Buffer = openFile(Path);
  if (Buffer)
    return Buffer;

  // FIXME: Should we sometimes return the error directly?
  consumeError(Buffer.takeError());
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "unknown blob '" + PrintableHash + "'");
}

StringRef BuiltinCAS::getPathForID(StringRef BaseDir, CASID ID,
                                   SmallVectorImpl<char> &Storage) {
  Storage.assign(RootPath->begin(), RootPath->end());
  sys::path::append(Storage, BaseDir);

  SmallString<64> PrintableHash;
  extractPrintableHash(ID, PrintableHash);
  sys::path::append(Storage, PrintableHash);

  return StringRef(Storage.begin(), Storage.size());
}

OnDiskCAS::OnDiskCAS(StringRef RootPath, OnDiskHashMappedTrie OnDiskObjects,
                     OnDiskHashMappedTrie OnDiskResults)
  : OnDiskObjects(std::move(OnDiskObjects)),
  OnDiskResults(std::move(OnDiskResults)), RootPath(RootPath.str()) {
    SmallString<128> Temp = RootPath;
    sys::path::append(Temp, "tmp.");
    TempPrefix = Temp.str().str();
  }

Expected<std::unique_ptr<CASDB>> cas::createOnDiskCAS(const Twine &Path) {
  SmallString<256> AbsPath;
  Path.toVector(AbsPath);
  sys::fs::make_absolute(AbsPath);

  if (AbsPath == getDefaultOnDiskCASStableID())
    AbsPath = StringRef(getDefaultOnDiskCASPath());

  if (std::error_code EC = sys::fs::create_directories(AbsPath)) {
    return createFileError(AbsPath, EC);
  }

  Optional<OnDiskHashMappedTrie> OnDiskObjects;
  Optional<OnDiskHashMappedTrie> OnDiskResults;

  uint64_t GB = 1024ull * 1024ull * 1024ull;
  if (Error E =
          OnDiskHashMappedTrie::create(AbsPath.str() + "/objects",
                                       sizeof(HashType) * 8, 16 * GB)
              .moveInto(OnDiskObjects))
    return std::move(E);
  if (Error E = OnDiskHashMappedTrie::create(AbsPath.str() + "/results",
                                             sizeof(HashType) * 8, GB)
                    .moveInto(OnDiskResults))
    return std::move(E);



  return std::make_unique<BuiltinCAS>(AbsPath, std::move(*OnDiskObjects),
                                      std::move(*OnDiskResults));

  // FIXME: This is a code dump from OnDiskHashMappedTrie::create(). Need to
  // fix and incorporate above.
  SmallString<128> DataPath = StringRef(Path);
  SmallString<128> IndexPath = StringRef(Path);
  sys::path::append(DataPath, "data");
  sys::path::append(IndexPath, "index");

  if (std::error_code EC = sys::fs::create_directory(Path))
    return errorCodeToError(EC);

  static_assert(sizeof(size_t) == sizeof(uint64_t), "64-bit only");
  static_assert(sizeof(std::atomic<int64_t>) == sizeof(uint64_t),
                "Requires lock-free 64-bit atomics");

  static constexpr uint64_t GB = 1024ull * 1024ull * 1024ull;
  static constexpr uint64_t MB = 1024ull * 1024ull;

  // Max out the index at 4GB. Take the limit directly for data.
  const uint64_t MaxIndexSize = std::min(4 * GB, MaxMapSize);
  const uint64_t MaxDataSize = MaxMapSize;

  // Open / create / initialize files on disk.
  std::shared_ptr<LazyMappedFileRegion> IndexRegion, DataRegion;
  if (Error E = LazyMappedFileRegion::createShared(
                    IndexPath, MaxIndexSize,
                    IndexFile::getConstructor(*InitialNumRootBits,
                                              *InitialNumSubtrieBits,
                                              NumHashBits, /*MinFileSize=*/MB))
                    .moveInto(IndexRegion))
    return std::move(E);
  if (Error E = LazyMappedFileRegion::createShared(
                    DataPath, MaxDataSize,
                    DataFile::getConstructor(/*MinFileSize=*/MB))
                    .moveInto(DataRegion))
    return std::move(E);

  // FIXME: The magic should be checked...

  // Success.
  OnDiskHashMappedTrie::ImplType Impl = {
      IndexFile(std::move(IndexRegion)),
      DataFile(std::move(DataRegion), std::move(Path)),
  };
  return OnDiskHashMappedTrie(std::make_unique<ImplType>(std::move(Impl)));
}

// FIXME: Proxy not portable. Maybe also error-prone?
constexpr StringLiteral DefaultDirProxy = "/^llvm::cas::builtin::default";
constexpr StringLiteral DefaultName = "llvm.cas.builtin.default";

void cas::getDefaultOnDiskCASPath(SmallVectorImpl<char> &Path) {
  if (!llvm::sys::path::cache_directory(Path))
    report_fatal_error("cannot get default cache directory");
  llvm::sys::path::append(Path, DefaultName);
}

std::string cas::getDefaultOnDiskCASPath() {
  SmallString<128> Path;
  getDefaultOnDiskCASPath(Path);
  return Path.str().str();
}

void cas::getDefaultOnDiskCASStableID(SmallVectorImpl<char> &Path) {
  Path.assign(DefaultDirProxy.begin(), DefaultDirProxy.end());
  llvm::sys::path::append(Path, DefaultName);
}

std::string cas::getDefaultOnDiskCASStableID() {
  SmallString<128> Path;
  getDefaultOnDiskCASStableID(Path);
  return Path.str().str();
}

//===----------------------------------------------------------------------===//
// old data handle stuff
/// TODO: Incorporate into BuiltinCAS.
//===----------------------------------------------------------------------===//
struct DataHandle {
  struct Header {
    unsigned HashSize : 16;
    unsigned HashPaddingSize : 8;
    unsigned MetadataPaddingSize : 8;
    unsigned MetadataSize : 31;
    unsigned IsEmbedded : 1;
    uint64_t DataSize;
  };

  // Files 64KB and bigger can just be saved separately.
  //
  // FIXME: Should be configurable.
  static constexpr int64_t MaxEmbeddedDataSize = 64ull * 1024ull - 1ull;

  // FIXME: This is being used as a hack to disable an assertion. The problem
  // is that if there are LOTS of references it the number above can exceed
  // MaxEmbeddedDataSize, even if the data itself is tiny. The right fix is to
  // sometimes (always?) make references external too.
  static constexpr int64_t MinExternalDataSize = 4ull * 1024ull - 1ull;

  static uint64_t getHashOffset() { return sizeof(DataHandle::Header); }

  static uint64_t getPadding(uint64_t Size) {
    if (uint64_t Remainder = Size % 8)
      return 8 - Remainder;
    return 0;
  }

  static uint64_t getHashPaddingStart(uint64_t HashSize) {
    return sizeof(Header) + HashSize;
  }

  static uint64_t getSizeIfExternal(uint64_t HashSize, uint64_t MetadataSize) {
    return sizeof(Header) + HashSize + getPadding(HashSize) + MetadataSize +
           getPadding(MetadataSize);
  }

  static uint64_t getDataPadding(uint64_t DataSize) {
    // Always at least one zero for guaranteed null termination.
    return getPadding(DataSize + 1) + 1;
  }

  static uint64_t getDataSizeWithPadding(uint64_t DataSize) {
    return DataSize + getDataPadding(DataSize);
  }

  static uint64_t getSizeIfEmbedded(ArrayRef<uint8_t> Hash, StringRef Metadata,
                                    StringRef Data) {
    return getSizeIfExternal(Hash.size(), Metadata.size()) +
           getDataSizeWithPadding(Data.size());
  }

  ArrayRef<uint8_t> getHash() const {
    return makeArrayRef(reinterpret_cast<const uint8_t *>(getHashStart()),
                        H->HashSize);
  }

  bool isEmbedded() const { return H->IsEmbedded; }
  bool isExternal() const { return !isEmbedded(); }

  const char *getHashStart() const {
    return reinterpret_cast<const char *>(H) + sizeof(Header);
  }
  const char *getHashPaddingStart() const {
    return getHashStart() + H->HashSize;
  }
  const char *getMetadataStart() const {
    return getHashPaddingStart() + H->HashPaddingSize;
  }
  const char *getMetadataPaddingStart() const {
    return getMetadataStart() + H->MetadataSize;
  }
  const char *getDataStart() const {
    return getMetadataPaddingStart() + H->MetadataPaddingSize;
  }
  const char *getDataPaddingStart() const {
    return getDataStart() + H->DataSize;
  }
  StringRef getMetadata() const {
    return StringRef(getMetadataStart(), H->MetadataSize);
  }

  Optional<StringRef> getData() const {
    if (isExternal())
      return None;
    return StringRef(getDataStart(), H->DataSize);
  }

  uint64_t getDataSizeWithPadding() const {
    return getDataSizeWithPadding(H->DataSize);
  }

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }

  SubtrieSlotValue getOffset() const {
    return SubtrieSlotValue::getDataOffset(reinterpret_cast<const char *>(H) -
                                           LMFR->data());
  }

  static DataHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                           StringRef DirPath, ArrayRef<uint8_t> Hash,
                           StringRef Metadata, StringRef Data);

  DataHandle() = default;
  DataHandle(LazyMappedFileRegion &LMFR, Header &H) : LMFR(&LMFR), H(&H) {}
  DataHandle(LazyMappedFileRegion &LMFR, SubtrieSlotValue Offset)
      : DataHandle(LMFR,
                   *reinterpret_cast<Header *>(LMFR.data() + Offset.asData())) {
  }

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;

  static DataHandle construct(LazyMappedFileRegion &LMFR, intptr_t Offset,
                              bool IsEmbedded, ArrayRef<uint8_t> Hash,
                              StringRef Metadata, uint64_t DataSize);
  static DataHandle createExternal(LazyMappedFileRegionBumpPtr &Alloc,
                                   ArrayRef<uint8_t> Hash, StringRef Metadata,
                                   uint64_t DataSize);
  static DataHandle createEmbedded(LazyMappedFileRegionBumpPtr &Alloc,
                                   ArrayRef<uint8_t> Hash, StringRef Metadata,
                                   StringRef Data);
};

DataHandle DataHandle::construct(LazyMappedFileRegion &LMFR, intptr_t Offset,
                                 bool IsEmbedded, ArrayRef<uint8_t> Hash,
                                 StringRef Metadata, uint64_t DataSize) {
  Header *H = new (LMFR.data() + Offset) Header{
      (unsigned)Hash.size(),
      (unsigned)getPadding(Hash.size()),
      (unsigned)getPadding(Metadata.size()),
      (unsigned)Metadata.size(),
      (unsigned)IsEmbedded,
      DataSize,
  };

  assert(Metadata.size() < (1u << 31) && "Too much metadata");
  DataHandle D(LMFR, *H);
  ::memcpy(const_cast<char *>(D.getHashStart()), Hash.begin(), Hash.size());
  ::memcpy(const_cast<char *>(D.getMetadataStart()), Metadata.begin(),
           Metadata.size());

  // Fill padding with 0s in case the filesystem doesn't guarantee it
  // (Windows).
  ::memset(const_cast<char *>(D.getHashPaddingStart()), 0,
           D.getHeader().HashPaddingSize);
  ::memset(const_cast<char *>(D.getMetadataPaddingStart()), 0,
           D.getHeader().MetadataPaddingSize);
  return D;
}

DataHandle DataHandle::createExternal(LazyMappedFileRegionBumpPtr &Alloc,
                                      ArrayRef<uint8_t> Hash,
                                      StringRef Metadata, uint64_t DataSize) {
  intptr_t Size = getSizeIfExternal(Hash.size(), Metadata.size());
  intptr_t Offset = Alloc.allocateOffset(Size);
  return construct(Alloc.getRegion(), Offset, /*IsEmbedded=*/false, Hash,
                   Metadata, DataSize);
}

DataHandle DataHandle::createEmbedded(LazyMappedFileRegionBumpPtr &Alloc,
                                      ArrayRef<uint8_t> Hash,
                                      StringRef Metadata, StringRef Data) {
  intptr_t Size = getSizeIfEmbedded(Hash, Metadata, Data);
  intptr_t Offset = Alloc.allocateOffset(Size);
  DataHandle D = construct(Alloc.getRegion(), Offset, /*IsEmbedded=*/true, Hash,
                           Metadata, Data.size());
  ::memcpy(const_cast<char *>(D.getDataStart()), Data.begin(), Data.size());

  // Fill padding with 0s in case the filesystem doesn't guarantee it
  // (Windows).
  ::memset(const_cast<char *>(D.getDataPaddingStart()), 0,
           DataHandle::getDataPadding(Data.size()));

  return D;
}

namespace {
OnDiskHashMappedTrie::MappedContentReference
DataFile::getContent(DataHandle D) const {
  if (D.isEmbedded())
    return OnDiskHashMappedTrie::MappedContentReference(D.getMetadata(),
                                                        *D.getData());

  SmallString<128> HashString;
  appendHash(D.getHash(), HashString);
  SmallString<256> ContentPath = StringRef(DirPath);
  sys::path::append(ContentPath, HashString);

  Expected<sys::fs::file_t> ContentFD =
      sys::fs::openNativeFileForRead(ContentPath, sys::fs::OF_None);
  if (!ContentFD)
    report_fatal_error(ContentFD.takeError());
  auto CloseOnReturn =
      llvm::make_scope_exit([&ContentFD]() { sys::fs::closeFile(*ContentFD); });

  // Load with the padding to guarantee null termination.
  int64_t ExternalSize = D.getDataSizeWithPadding();
  sys::fs::mapped_file_region Map;
  {
    std::error_code EC;
    Map = sys::fs::mapped_file_region(
        sys::fs::convertFDToNativeFile(*ContentFD),
        sys::fs::mapped_file_region::readonly, ExternalSize, 0, EC);
    if (EC)
      report_fatal_error(createFileError(ContentPath, EC));
  }

  StringRef Content(Map.data(), D.getHeader().DataSize);
  return OnDiskHashMappedTrie::MappedContentReference(D.getMetadata(), Content,
                                                      std::move(Map));
}

DataHandle DataHandle::create(LazyMappedFileRegionBumpPtr &Alloc,
                              StringRef DirPath, ArrayRef<uint8_t> Hash,
                              StringRef Metadata, StringRef Data) {
  int64_t Size = getSizeIfEmbedded(Hash, Metadata, Data);

  // FIXME: This logic isn't right. References should be stored externally if
  // there are enough that we'll go over the max embedded data size limit.
  // Right now only the data is ever made external.... but we could blow out
  // the main mmap if there are lots of references.
  if (Size <= MaxEmbeddedDataSize || Data.size() < MinExternalDataSize)
    return createEmbedded(Alloc, Hash, Metadata, Data);

  createExternalContent(DirPath, Hash, Metadata, Data);
  return createExternal(Alloc, Hash, Metadata, Data.size());
}

static void appendHash(ArrayRef<uint8_t> RawHash, SmallVectorImpl<char> &Dest) {
  assert(Dest.empty());
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


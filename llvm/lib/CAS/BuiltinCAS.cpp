//===- BuiltinCAS.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/HashMappedTrie.h"
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

/// Trie record data: 8B, atomic<uint64_t>
/// - 1-byte: StorageKind
/// - 1-byte: ObjectKind
/// - 6-bytes: DataStoreOffset (offset into referenced file)
struct TrieRecord {
  enum class StorageKind : uint8_t {
    /// Unknown object.
    Unknown = 0,

    /// v1.data: in the main pool, with a full DataStore record.
    Default = 1,

    /// v1.<TrieRecordOffset>.data: standalone, with a full DataStore record.
    Standalone = 2,

    /// v1.<TrieRecordOffset>.blob: standalone, just the data. File contents
    /// exactly the data content and file size matches the data size. No refs.
    StandaloneBlob = 3,

    /// v1.<TrieRecordOffset>.blob+0: standalone, just the data plus an
    /// extra null character ('\0'). File size is 1 bigger than the data size.
    /// No refs.
    StandaloneBlobNull = 4,
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
    // Saves files bigger than 64KB standalone.
    MaxEmbeddedSize = 64LL * 1024LL - 1,
  };

  struct Data {
    StorageKind SK = StorageKind::SK;
    ObjectKind OK = ObjectKind::Node;
    uint64_t Offset = 0;
  };

  static uint64_t pack(Data D) {
    assert(D.Offset < (1ULL << 48));
    uint64_t Packed = uint64_t(D.SK) << 56 | uint64_t(D.OK) << 48 | D.Offset;
    assert(D.SK != StorageKind::Unknown || Packed == 0);
    return Packed;
  }

  static Data unpack(uint64_t Packed) {
    Data D;
    if (!Packed)
      return D;
    D.SK = (StorageKind)(Packed >> 56);
    D.OK = (ObjectKind)((Packed >> 48) & 0xFF);
    D.Offset = Packed & (-1ULL >> 48);
    return D;
  }

  TrieRecord() : PackedOffset(0) {}

  std::atomic<uint64_t> PackedOffset;
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
  InternalRef4B(uint32_t Data) : Data(Data);
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
      return Ref;
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
    InternalRef Ref;
  };

  iterator begin() const {
    if (auto *Ref = Begin.dyn_cast<const InternalRef4B *>())
      return iterator(Ref);
    if (auto *Ref = Begin.dyn_cast<const InternalRef *>())
      return iterator(ref);
    return iterator(nullptr);
  }
  iterator end() const { return begin() + Size; }

  /// Array accessor.
  ///
  /// Returns a reference proxy to avoid lifetime issues, since a reference
  /// derived from a InternalRef4B lives inside the iterator.
  iterator::ReferenceProxy operator[](ptrdiff_t N) const { return begin()[N]; }

  Optional<ArrayRef<InternalRef>> getAs8B() const {
    if (Is4B)
      return None;
    return ArrayRef<InternalRef>(reinterpret_cast<const InternalRef *>(Ref),
                                 Size);
  }

  Optional<ArrayRef<InternalRef4B>> getAs4B() const {
    if (!Is4B)
      return None;
    return ArrayRef<InternalRef>(reinterpret_cast<const InternalRef4B *>(Ref),
                                 Size);
  }

  InternalRefArrayRef() = default;

  InternalRefArrayRef(ArrayRef<InternalRef> Refs)
      : Begin(Refs.begin()), Size(Refs.size()), RefSize(sizeof(*Refs.begin())) {}

  InternalRefArrayRef(ArrayRef<InternalRef4B> Refs)
      : Begin(Refs.begin()), Size(Refs.size()), RefSize(sizeof(*Refs.begin())) {}

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
  enum class LayoutFlags : uint8_t {
    Init = 0x0,

    /// NumRefs storage: 4B, 2B, 1B, or 0B (no refs). Or, 8B, for alignment
    /// convenience to avoid computing padding later.
    NumRefsMask = 0x03U << 0,
    NumRefs0B = 0U << 0,
    NumRefs1B = 1U << 0,
    NumRefs2B = 2U << 0,
    NumRefs4B = 3U << 0,
    NumRefs8B = 4U << 0,

    /// DataSize storage: 8B, 4B, 2B, or 1B.
    DataSizeMask = 0x03U << 3,
    DataSize1B = 0U << 2,
    DataSize2B = 1U << 2,
    DataSize4B = 2U << 2,
    DataSize8B = 3U << 2,

    /// TrieRecord storage: 6B or 4B.
    TrieOffsetMask = 0x01U << 5,
    TrieOffset6B = 0U << 4,
    TrieOffset4B = 1U << 4,

    /// Refs[] storage: 8B or 4B.
    RefSizeMask = 0x01U << 6,
    RefSize8B = 0U << 0,
    RefSize4B = 1U << 0,

    friend LayoutFlags operator|(LayoutFlags, LayoutFlags) = default;
    friend LayoutFlags operator&(LayoutFlags, LayoutFlags) = default;
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
    uint64_t TrieRecordOffset;
    ArrayRef<char> Data;
    ArrayRef<InternalRef> Refs;
  };

  uint64_t getLayoutFlags() const { return (LayoutFlags)(H.Packed >> 56); }
  uint64_t getTrieRecordOffset() const {
    LayoutFlags TrieOffsetBits = getLayoutFlags() & LayoutFlags::TrieOffsetMask;
    if (TrieOffsetBits == LayoutFlags::TrieOffset4B)
      return H->Packed & UINT32_MAX;
    return H->Packed & (UINT64_MAX >> 16);
  }

  uint64_t getDataSize() const;
  void skipDataSize(int64_t &RelOffset) const;
  uint32_t getNumRefs() const;
  void skipNumRefs(int64_t &RelOffset) const;
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

    LayoutFlags Flags = LayoutFlags::Init;
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
    LayoutFlags RefSizeBits = getLayoutFlags() & LayoutFlags::RefSizeMask;
    if (RefSizeBits == LayoutFlags::RefSize4B)
      return makeArrayRef(reinterpret_cast<const InternalRef4B *>(BeginByte),
                          Size);
    return makeArrayRef(reinterpret_cast<const InternalRef *>(BeginByte), Size);
  }

  ArrayRef<char> getData() const {
    assert(H && "Expected valid handle");
    return makeArrayRef(reinterpret_cast<const char *>(H) + getDataRelOffset(),
                        getDataSize());
  }

  static DataRecordHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                                 const Input &I);
  static DataRecordHandle construct(char *Mem, const Input &I);

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }

  DataRecordHandle() = default;

private:
  static DataRecordHandle constructImpl(char *Mem, const Input &I, const Layout &L);
  DataRecordHandle(const Header &H) : H(&H) {}
  const Header *H = nullptr;
};

DataRecordHandle DataRecordHandle::create(LazyMappedFileRegionBumpPtr &Alloc,
                                          const Input &I) {
  Layout L(I);
  return construct(Alloc.allocate(L.getTotalSize()), I, L);
}

/// On-disk or in-memory.
///
/// On-disk version uses the database currently described in
/// OnDiskHashMappedTrie.h.
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
/// Later:
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
class BuiltinCAS : public CASDB {
public:
  class TempFile;

  using MappedContentReference = OnDiskHashMappedTrie::MappedContentReference;
  struct ContentReference {
    StringRef Metadata;
    StringRef Data;

    ContentReference() = delete;
    ContentReference(StringRef Metadata, StringRef Data)
        : Metadata(Metadata), Data(Data) {}
  };

  /// How objects are stored in memory.
  struct ObjectContentReference {
    StringRef ReferenceBlock;
    StringRef Data;

    size_t getNumReferences() const {
      return ReferenceBlock.size() / NumHashBytes;
    }
    CASID getReference(size_t I) const;
    static Optional<ObjectContentReference>
    getFromContent(ContentReference Content);
  };

  /// A CASID plus a reference to an object stored in memory.
  struct ObjectAndID {
    const ObjectContentReference &Object;
    CASID ID;
  };

  /// List of references, either as CASIDs or a contiguous block.
  struct AbstractReferenceList {
    size_t getNumBytes() const;
    size_t getNumReferences() const;
    void appendTo(SmallVectorImpl<char> &Sink) const;
    void updateHash(SHA1 &Hasher) const;

    AbstractReferenceList() = delete;
    AbstractReferenceList(StringRef ReferenceBlock)
        : ReferenceBlock(ReferenceBlock) {}
    AbstractReferenceList(ArrayRef<CASID> ReferenceList)
        : ReferenceList(ReferenceList) {}
    AbstractReferenceList(NoneType) : ReferenceList(ArrayRef<CASID>()) {}

    Optional<StringRef> ReferenceBlock;
    Optional<ArrayRef<CASID>> ReferenceList;
  };

  struct TreeObjectData;

  /// An object, which may or may not exist yet.
  struct AbstractObjectReference {
    AbstractReferenceList References;
    StringRef Data;

    // Return combined references and data.
    StringRef build(SmallVectorImpl<char> &Metadata,
                    SmallVectorImpl<char> &DataStorage) const;
    HashType computeHash() const;

    AbstractObjectReference(AbstractReferenceList References, StringRef Data)
        : References(References), Data(Data) {}
    AbstractObjectReference(ObjectContentReference Object)
        : References(Object.ReferenceBlock), Data(Object.Data) {}
    AbstractObjectReference(const TreeObjectData &Object);
  };

  Expected<CASID> parseCASID(StringRef Reference) final;
  Error printCASID(raw_ostream &OS, CASID ID) final;

  Expected<ObjectAndID>
  createObject(AbstractObjectReference Object,
               Optional<sys::fs::mapped_file_region> NullTerminatedMap = None);

  Expected<BlobRef> createBlob(StringRef Data) final {
    return createBlobImpl(Data, None);
  }
  Expected<BlobRef> createBlobImpl(
      StringRef Data,
      Optional<sys::fs::mapped_file_region> NullTerminatedMap = None);
  Expected<TreeRef> createTree(ArrayRef<NamedTreeEntry> Entries = None) final;
  bool isKnownObject(CASID ID) final;
  Optional<ObjectKind> getObjectKind(CASID ID) final;

  Expected<BlobRef>
  createBlobFromOpenFileImpl(sys::fs::file_t FD,
                             Optional<sys::fs::file_status> Status) override;

  Expected<ObjectAndID> getObject(CASID ID);
  Expected<BlobRef> getBlob(CASID ID) final {
    return getBlobFromObject(getObject(ID));
  }

  Expected<NodeRef> getNodeFromObject(Expected<ObjectAndID> Object);
  Expected<NodeRef> createNode(ArrayRef<CASID> References,
                               StringRef Data) final;
  Expected<NodeRef> getNode(CASID ID) final {
    return getNodeFromObject(getObject(ID));
  }

  static Expected<BlobRef> getBlobFromObject(Expected<ObjectAndID> Object);

  Expected<TreeRef> getTreeFromObject(Expected<ObjectAndID> Object);

  Expected<TreeRef> getTree(CASID ID) final {
    return getTreeFromObject(getObject(ID));
  }

  bool isInMemoryOnly() const { return !RootPath; }
  bool isOnDisk() const { return !isInMemoryOnly(); }

  void print(raw_ostream &OS) const final;

  Expected<CASID> getCachedResult(CASID InputID) final;
  Error putCachedResult(CASID InputID, CASID OutputID) final;

  struct InMemoryOnlyTag {};
  BuiltinCAS() = delete;
  explicit BuiltinCAS(InMemoryOnlyTag) { AlignedInMemoryStrings.emplace(); }
  explicit BuiltinCAS(StringRef RootPath, OnDiskHashMappedTrie OnDiskObjects,
                      OnDiskHashMappedTrie OnDiskResults)
      : OnDiskObjects(std::move(OnDiskObjects)),
        OnDiskResults(std::move(OnDiskResults)), RootPath(RootPath.str()) {
    SmallString<128> Temp = RootPath;
    sys::path::append(Temp, "tmp.");
    TempPrefix = Temp.str().str();
  }

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

  // Other stuff.

  ContentReference getOrCreatePersistentContent(
      HashRef Hash, StringRef Metadata, StringRef Data,
      Optional<sys::fs::mapped_file_region> NullTerminatedDataMap = None);

  /// Return a version of \p Data that is persistent.
  ///
  /// If \p NullTerminatedMap is sent in, it must be a valid, null-terminated
  /// map that \p Data points to. It will be stored in memory and \p Data
  /// returned.
  ///
  /// Otherwise, a copy of \p Data will be bump-ptr-allocated and returned.
  StringRef persistBufferInMemory(
      StringRef Buffer, bool ShouldNullTerminate,
      Optional<sys::fs::mapped_file_region> NullTerminatedMap);

  StringRef persistMetadataInMemory(StringRef Metadata) {
    return persistBufferInMemory(Metadata, /*ShouldNullTerminate=*/false, None);
  }

  StringRef persistDataInMemory(
      StringRef Data,
      Optional<sys::fs::mapped_file_region> NullTerminatedMap = None) {
    return persistBufferInMemory(Data, /*ShouldNullTerminate=*/true,
                                 std::move(NullTerminatedMap));
  }

  /// Return the buffer from \p Content after making the map (if any)
  /// persistent.
  ContentReference
  persistMappedContentInMemory(MappedContentReference Content) {
    return ContentReference(
        Content.Metadata,
        Content.Map ? persistDataInMemory(Content.Data, std::move(Content.Map))
                    : Content.Data);
  }

  StringRef getPathForID(StringRef BaseDir, CASID ID,
                         SmallVectorImpl<char> &Storage);

  Expected<std::unique_ptr<MemoryBuffer>> openFile(StringRef Path);
  Expected<std::unique_ptr<MemoryBuffer>> openFileWithID(StringRef BaseDir,
                                                         CASID ID);

  Optional<MappedContentReference> openOnDisk(OnDiskHashMappedTrie &Trie,
                                              HashRef Hash);

  Optional<OnDiskHashMappedTrie> OnDiskObjects;
  Optional<OnDiskHashMappedTrie> OnDiskResults;

  using ObjectCacheType =
      ThreadSafeHashMappedTrie<ObjectContentReference, sizeof(HashType)>;
  ObjectCacheType ObjectCache;

  using ResultCacheType = ThreadSafeHashMappedTrie<HashType, sizeof(HashType)>;
  ResultCacheType ResultCache;

  Optional<std::string> RootPath;
  Optional<std::string> TempPrefix;

  Optional<ThreadSafeAllocator<SpecificBumpPtrAllocator<char>>>
      AlignedInMemoryStrings;
  ThreadSafeAllocator<SpecificBumpPtrAllocator<HashType>> ParsedIDs;
  ThreadSafeAllocator<SpecificBumpPtrAllocator<sys::fs::mapped_file_region>>
      PersistentMaps;
};

} // namespace

DataRecordHandle DataRecordHandle::construct(char *Mem, const Input &I) {
  return construct(Mem, I, Layout(I));
}

DataRecordHandle DataRecordHandle::constructImpl(char *Mem, const Input &I,
                                                 const Layout &L) {
  uint64_t Packed = 0;
  Packed |= (L.Flags << 56);

  // Fill in Packed and set other data, then come back to construct the header.
  char *Next = Mem + sizeof(Header);
  switch (L.Flags & LayoutFlags::DataSizeMask) {
  case LayoutFlags::DataSize1B:
    H->Packed |= (uint64_t)I.Data.size() << 48;
    break;
  case LayoutFlags::DataSize2B:
    H->Packed |= (uint64_t)I.Data.size() << 32;
    break;
  case LayoutFlags::DataSize4B:
    assert(isAligned(Align(4), Next - Mem));
    new (Next) uint32_t(I.Data.size());
    Next += 4;
    break;
  case LayoutFlags::DataSize8B:
    assert(isAligned(Align(8), Next - Mem));
    new (Next) uint64_t(I.Data.size());
    Next += 8;
    break;
  }

  // NOTE: May need to write NumRefs even if there are none as a trick to get
  // alignment later.
  char *Next = Mem + sizeof(Header);
  switch (L.Flags & LayoutFlags::NumRefsMask) {
  case LayoutFlags::NumRefs0B:
    break;
  case LayoutFlags::NumRefs1B:
    H->Packed |= (uint64_t)I.Refs.size() << 48;
    break;
  case LayoutFlags::NumRefs2B:
    H->Packed |= (uint64_t)I.Refs.size() << 32;
    break;
  case LayoutFlags::NumRefs4B:
    assert(isAligned(Align(4), Next - Mem));
    new (Next) uint32_t(I.Refs.size());
    Next += 4;
    break;
  case LayoutFlags::NumRefs8B:
    assert(isAligned(Align(8), Next - Mem));
    new (Next) uint64_t(I.Refs.size());
    Next += 8;
    break;
  }

  if (!I.Refs.empty()) {
    if ((L.Flags & LayoutFlags::RefSizeMask) == LayoutFlags::RefSize4B) {
      assert(isAligned(Align(4), Next - Mem));
      auto *Ref = new (Next) InternalRef4B[I.Refs.size()];
      for (auto RI = I.Refs.begin(), RE = I.Refs.end(); RI != RE; ++RI)
        *Ref = *RI->shrinkTo4B();
      Next += 4 * I.Refs.size();
    } else {
      assert(isAligned(Align(8), Next - Mem));
      auto *Ref = new (Next) InternalRef[I.Refs.size()];
      llvm::copy(I.Refs, Ref);
      Next += 8 * I.Refs.size();
    }
  }

  // Write the data and trailing null.
  assert(isAligned(Align(8), Next - Mem));
  llvm::copy(I.Data, Next);
  Next[I.Data.size()] = 0;

  // Construct the header and return.
  Header *H = new (Mem) Header{Packed};
  return DataRecordHandle(*H);
}

DataRecordHandle::Layout::Layout(const Input &I) const {
  Layout L;

  // Start initial relative offsets right after the Header.
  uint64_t RelOffset = sizeof(uint64_t);

  // Initialize the easy stuff.
  L.DataSize = I.Data.size();
  L.NumRefs = I.Refs.size();

  // Check refs size.
  LayoutFlags RefSizeBits = LayoutFlags::RefSize4B;
  if (L.NumRefs) {
    for (uint64_t Ref : L.Refs) {
      if (Ref > UINT32_MAX) {
        RefSizeBits = LayoutFlags::RefSize8B;
        break;
      }
    }
  }

  // Set the trie offset.
  assert(TrieRecordOffset <= (UINT64_MAX >> 16));
  L.TrieRecordOffset = TrieRecordOffset;
  LayoutFlags TrieOffsetBits = TrieRecordOffset <= UINT32_MAX
      ? LayoutFlags::TrieOffset4B
      : LayoutFlags::TrieOffset6B;

  // Find the smallest slot available for DataSize.
  bool Has1B = true;
  bool Has2B = TrieOffsetBits == LayoutFlags::TrieOffset4B;
  LayoutFlags DataSizeBits;
  if (L.DataSize <= UINT8_MAX && Has1B) {
    DataSizeBits = LayoutFlags::DataSize1B;
    Has1B = false;
  } else if (L.DataSize <= UINT16_MAX && Has2B) {
    DataSizeBits = LayoutFlags::DataSize2B;
    Has2B = false;
  } else if (L.DataSize <= UINT32_MAX) {
    DataSizeBits = LayoutFlags::DataSize4B;
    RelOffset += 4;
  } else {
    DataSizeBits = LayoutFlags::DataSize8B;
    RelOffset += 8;
  }

  // Find the smallest slot available for NumRefs. Never sets NumRefs8B here.
  LayoutFlags NumRefsBits;
  if (!L.NumRefs) {
    NumRefsBits = LayoutFlags::NumRefs0B;
  } else if (L.NumRefs <= UINT8_MAX && Has1B) {
    NumRefsBits = LayoutFlags::NumRefs1B;
    Has1B = false;
  } else if (L.NumRefs <= UINT16_MAX && Has2B) {
    NumRefsBits = LayoutFlags::NumRefs2B;
    Has2B = false;
  } else {
    NumRefsBits = LayoutFlags::NumRefs4B;
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

    assert(NumRefsBits != LayoutFlags::NumRefs8B &&
           "Expected to be able to grow NumRefs8B");

    // First try to grow DataSize. NumRefs will not (yet) be 8B, and if
    // DataSize is upgraded to 8B it'll already be aligned.
    //
    // Failing that, grow NumRefs.
    if (DataSizeBits < LayoutFlags::DataSize4B)
      DataSizeBits = LayoutFlags::DataSize4B; // DataSize: Packed => 4B.
    else if (DataSizeBits < LayoutFlags::DataSize8B)
      DataSizeBits = LayoutFlags::DataSize8B; // DataSize: 4B => 8B.
    else if (NumRefsBits < LayoutFlags::NumRefs4B)
      NumRefsBits = LayoutFlags::NumRefs4B; // NumRefs: Packed => 4B.
    else
      NumRefsBits = LayoutFlags::NumRefs8B; // NumRefs: 4B => 8B.
  };

  assert(isAligned(Align(4), RelOffset));
  if (RefSizeBits == LayoutFlags::RefSize8B) {
    // List of 8B refs should be 8B-aligned. Grow one of the sizes to get this
    // without padding.
    if (!isAligned(Align(8), RelOffset))
      GrowSizeFieldsBy4B();

    assert(isAligned(Align(8), RelOffset));
    L.RefsRelOffset = RelOffset;
    RelOffset += 8 * L.NumRefs;
  } else {
    // The array of 4B refs doesn't need 8B alignment, but the data will need
    // to be 8B-aligned. Detect this now, and, if necessary, shift everything
    // by 4B by growing one of the sizes.
    uint64_t RefListSize = 4 * L.NumRefs;
    if (!isAligned(Align(8), RelOffset + RefListSize))
      GrowSizeFieldsBy4B();
    L.RefsRelOffset = RelOffset;
    RelOffset += RefListSize;
    RelOffset += 4 * L.NumRefs;
  }

  assert(isAligned(Align(8), RelOffset));
  L.DataRelOffset = RelOffset;
  L.Flags = NumRefsBits | RefSizeBits | DataSizeBits | TrieOffsetBits;
  return L;
}

uint64_t DataRecordHandle::getDataSize() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  auto *DataSizePtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (L.Flags & LayoutFlags::DataSizeMask) {
  case LayoutFlags::DataSize1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case LayoutFlags::DataSize2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case LayoutFlags::DataSize4B:
    return reinterpret_cast<const uint32_t *>(DataSizePtr);
  case LayoutFlags::DataSize8B:
    return reinterpret_cast<const uint64_t *>(DataSizePtr);
  }
}

void DataRecordHandle::skipDataSize(int64_t &RelOffset) const {
  LayoutFlags LF = getLayoutFlags();
  LayoutFlags DataSizeBits = LF.Flags & LayoutFlags::DataSizeMask;
  if (DataSizeBits >= LayoutFlags::DataSize4B)
    RelOffset += 4;
  if (DataSizeBits >= LayoutFlags::DataSize8B)
    RelOffset += 4;
}

uint32_t DataRecordHandle::getNumRefs() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  auto *NumRefsPtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (L.Flags & LayoutFlags::NumRefsMask) {
  case LayoutFlags::NumRefs0B:
    return 0;
  case LayoutFlags::NumRefs1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case LayoutFlags::NumRefs2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case LayoutFlags::NumRefs4B:
    return reinterpret_cast<const uint32_t *>(NumRefsPtr);
  case LayoutFlags::NumRefs8B:
    return reinterpret_cast<const uint64_t *>(NumRefsPtr);
  }
}

void DataRecordHandle::skipNumRefs(int64_t &RelOffset) const {
  LayoutFlags LF = getLayoutFlags();
  LayoutFlags NumRefsBits = LF.Flags & LayoutFlags::NumRefsMask;
  if (NumRefsBits >= LayoutFlags::NumRefs4B)
    RelOffset += 4;
  if (NumRefsBits >= LayoutFlags::NumRefs8B)
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
  uint32_t RefSize = LF & LayoutFlags::RefSize4B ? 4 : 8;
  RelOffset += RefSize * getNumRefs();
  return RelOffset;
}


using TreeObjectData = BuiltinCAS::TreeObjectData;
using ObjectContentReference = BuiltinCAS::ObjectContentReference;
using ObjectAndID = BuiltinCAS::ObjectAndID;
using ContentReference = BuiltinCAS::ContentReference;
using AbstractReferenceList = BuiltinCAS::AbstractReferenceList;
using AbstractObjectReference = BuiltinCAS::AbstractObjectReference;

template <typename T> static T reportAsFatalIfError(Expected<T> ValOrErr) {
  if (!ValOrErr)
    report_fatal_error(ValOrErr.takeError());
  return std::move(*ValOrErr);
}

static void reportAsFatalIfError(Error E) {
  if (E)
    report_fatal_error(std::move(E));
}

static HashRef bufferAsRawHash(StringRef Bytes) {
  assert(Bytes.size() == NumHashBytes);
  return HashRef(reinterpret_cast<const uint8_t *>(Bytes.begin()),
                 Bytes.size());
}

static StringRef rawHashAsBuffer(HashRef Hash) {
  assert(Hash.size() == NumHashBytes);
  return StringRef(reinterpret_cast<const char *>(Hash.begin()), Hash.size());
}

static HashType makeHash(HashRef Bytes) {
  assert(Bytes.size() == NumHashBytes);
  HashType Hash;
  ::memcpy(Hash.begin(), Bytes.begin(), Bytes.size());
  return Hash;
}

static HashType makeHash(CASID ID) { return makeHash(ID.getHash()); }

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

static StringRef getCASIDPrefix() { return "~{CASFS}:"; }

Expected<CASID> BuiltinCAS::parseCASID(StringRef Reference) {
  if (!Reference.consume_front(getCASIDPrefix()))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid cas-id '" + Reference + "'");

  // FIXME: Allow shortened references?
  if (Reference.size() != 2 * NumHashBytes)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "wrong size for cas-id hash '" + Reference + "'");

  // FIXME: Take parsing as a hint that the ID will be loaded and do a look up
  // of blobs and trees, rather than always allocating space for a hash.
  return CASID(*new (ParsedIDs.Allocate()) HashType(stringToHash(Reference)));
}

Error BuiltinCAS::printCASID(raw_ostream &OS, CASID ID) {
  if (ID.getHash().size() != NumHashBytes)
    return errorCodeToError(std::make_error_code(std::errc::invalid_argument));

  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  OS << getCASIDPrefix() << Hash;
  return Error::success();
}

void BuiltinCAS::print(raw_ostream &OS) const {
  if (RootPath)
    OS << "on-disk-root-path: " << *RootPath << "\n";
  OS << "in-memory-objects: ";
  ObjectCache.print(OS);
  OS << "in-memory-results: ";
  ResultCache.print(OS);
}

static Error createUnknownObjectError(CASID ID) {
  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "unknown object '" + Hash + "'");
}

static Error createCorruptObjectError(CASID ID) {
  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "corrupt object '" + Hash + "'");
}

static Error createInvalidObjectError(CASID ID, StringRef Kind) {
  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "invalid object '" + Hash + "' for kind '" + Kind + "'");
}

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
    assert(Entry.getID().getHash().size() == NumHashBytes);
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
      bufferAsRawHash(ReferenceBlock.substr(I * NumHashBytes, NumHashBytes)));
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
                        : ReferenceList->size() * NumHashBytes;
}

size_t BuiltinCAS::AbstractReferenceList::getNumReferences() const {
  return ReferenceBlock ? ReferenceBlock->size() / NumHashBytes
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
    assert(Hash.size() == NumHashBytes);
    Sink.append(Hash.begin(), Hash.end());
  }
}

void BuiltinCAS::AbstractReferenceList::updateHash(SHA1 &Hasher) const {
  if (ReferenceBlock) {
    Hasher.update(*ReferenceBlock);
    return;
  }
  for (CASID ID : *ReferenceList) {
    assert(ID.getHash().size() == NumHashBytes);
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
  uint64_t ReferenceBlockSize = NumReferences * NumHashBytes;
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
  HashRef Hash = ID.getHash();
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

Expected<NodeRef> BuiltinCAS::createNode(ArrayRef<CASID> References,
                                         StringRef Data) {
  return getNodeFromObject(
      createObject(AbstractObjectReference(References, Data)));
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

Expected<BlobRef>
BuiltinCAS::createBlobFromOpenFileImpl(sys::fs::file_t FD,
                                       Optional<sys::fs::file_status> Status) {
#if defined(__CYGWIN__)
  static int PageSize = 4096;
#else
  static int PageSize = sys::Process::getPageSizeEstimate();
#endif

  if (!Status) {
    Status.emplace();
    if (std::error_code EC = sys::fs::status(FD, *Status))
      return errorCodeToError(EC);
  }

  // Check whether we can trust the size from stat.
  Optional<uint64_t> StatusFileSize;
  if (Status->type() == sys::fs::file_type::regular_file ||
      Status->type() == sys::fs::file_type::block_file)
    StatusFileSize = Status->getSize();

  sys::fs::mapped_file_region MappedContent;
  SmallString<sys::fs::DefaultReadChunkSize> StreamedContent;
  Optional<StringRef> Content;
  if (StatusFileSize && *StatusFileSize > 4u * 4096) {
    std::error_code EC;
    MappedContent = sys::fs::mapped_file_region(
        FD, sys::fs::mapped_file_region::readonly, *StatusFileSize,
        /*offset=*/0, EC);
    if (EC)
      return errorCodeToError(EC);
    Content.emplace(MappedContent.data(), MappedContent.size());
  } else {
    if (Error E = sys::fs::readNativeFileToEOF(FD, StreamedContent))
      return std::move(E);
    Content.emplace(StreamedContent);
  }

  // If we used mmap, check if it's actually null-terminated. Note that the
  // contents could have changed between stat and open if this file is
  // volatile, so we need to check for a null character.
  //
  // If the map *is* null-terminated, pass it in to allow an in-memory trie to
  // use the file-backed storage (in case we aren't file-backed ourselves).
  Optional<sys::fs::mapped_file_region> NullTerminatedMap;
  if (MappedContent && (*StatusFileSize & (PageSize - 1)) &&
      Content->end()[0] == 0)
    NullTerminatedMap = std::move(MappedContent);

  return createBlobImpl(*Content, std::move(NullTerminatedMap));
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

ContentReference BuiltinCAS::getOrCreatePersistentContent(
    HashRef Hash, StringRef Metadata, StringRef Data,
    Optional<sys::fs::mapped_file_region> NullTerminatedDataMap) {
  assert(isInMemoryOnly() == !OnDiskObjects);
  if (OnDiskObjects)
    return persistMappedContentInMemory(
        OnDiskObjects->insert(Hash, Metadata, Data));
  return ContentReference(
      persistMetadataInMemory(Metadata),
      persistDataInMemory(Data, std::move(NullTerminatedDataMap)));
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

static Error createResultCacheMissError(CASID InputID) {
  SmallString<64> InputHash;
  extractPrintableHash(InputID, InputHash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "no result for '" + InputHash + "'");
}

static Error createResultCachePoisonedError(CASID InputID, CASID OutputID,
                                            CASID ExistingOutputID) {
  SmallString<64> InputHash, OutputHash, ExistingOutputHash;
  extractPrintableHash(InputID, InputHash);
  extractPrintableHash(OutputID, OutputHash);
  extractPrintableHash(ExistingOutputID, ExistingOutputHash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "cache poisoned for '" + InputHash + "' (new='" +
                               OutputHash + "' vs. existing '" +
                               ExistingOutputHash + "')");
}

static Error createResultCacheCorruptError(CASID InputID) {
  SmallString<64> InputHash;
  extractPrintableHash(InputID, InputHash);
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "result cache corrupt for '" + InputHash + "'");
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
  if (File->Data.size() != NumHashBytes)
    reportAsFatalIfError(createResultCacheCorruptError(InputID));
  return CASID(bufferAsRawHash(File->Data));
}

Error BuiltinCAS::putCachedResult(CASID InputID, CASID OutputID) {
  // Helper for confirming cached results are consistent.
  auto checkOutput = [&](HashRef ExistingOutput) -> Error {
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
  HashRef OutputHash = OutputID.getHash();
  MappedContentReference File =
      OnDiskResults->insert(InputHash, "results", rawHashAsBuffer(OutputHash));
  assert(!File.Map && "Unexpected mapped_file_region in result cache");
  return checkOutput(bufferAsRawHash(File.Data));
}

Optional<BuiltinCAS::MappedContentReference>
BuiltinCAS::openOnDisk(OnDiskHashMappedTrie &Trie, HashRef Hash) {
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
                                       NumHashBytes * 8, 16 * GB)
              .moveInto(OnDiskObjects))
    return std::move(E);
  if (Error E = OnDiskHashMappedTrie::create(AbsPath.str() + "/results",
                                             NumHashBytes * 8, GB)
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

std::unique_ptr<CASDB> cas::createInMemoryCAS() {
  return std::make_unique<BuiltinCAS>(BuiltinCAS::InMemoryOnlyTag());
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

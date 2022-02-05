//===- OnDiskHashMappedTrie.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/OnDiskHashMappedTrie.h"
#include "HashMappedTrieIndexGenerator.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CAS/LazyMappedFileRegion.h"
#include "llvm/CAS/LazyMappedFileRegionBumpPtr.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::cas;

static_assert(sizeof(size_t) == sizeof(uint64_t), "64-bit only");
static_assert(sizeof(std::atomic<int64_t>) == sizeof(uint64_t),
              "Requires lock-free 64-bit atomics");

//===----------------------------------------------------------------------===//
// Generic database data structures.
//===----------------------------------------------------------------------===//
namespace {
/// Generic handle for a table.
///
/// Probably we want some table kinds for pointing at multiple tables.
/// - Probably a tree or trie type makes sense.
/// - Or a deque. Linear search is okay as long as there aren't many tables in
///   a file.
///
/// Generic table header layout:
/// - 2-bytes: TableKind
/// - 2-bytes: TableNameSize
/// - 4-bytes: TableNameRelOffset (relative to header)
class TableHandle {
public:
  enum class TableKind : uint16_t {
    HashMappedTrie = 1,
    DataSink = 2,
  };
  struct Header {
    TableKind Kind;
    uint16_t NameSize;
    int32_t NameRelOffset; // Relative to Header.
  };

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }
  LazyMappedFileRegion &getRegion() const { return *LMFR; }

  template <class T> bool is() const { return T::Kind == H->Kind; }
  template <class T> T dyn_cast() const {
    if (is<T>())
      return T(*LMFR, *static_cast<T::Header *>(H));
    return T();
  }
  template <class T> T get() const {
    assert(is<T>());
    return T(*LMFR, *static_cast<T::Header *>(H));
  }

  StringRef getName() const {
    auto *Begin = reinterpret_cast<const char *>(H) + H->NameRelOffset;
    return StringRef(Begin, Begin + H->NameSize);
  }

  TableHandle(LazyMappedFileRegion &LMFR, Header &H) : LMFR(&LMFR), H(&H) {}
  TableHandle(LazyMappedFileRegion &LMFR, intptr_t HeaderOffset)
      : TableHandle(LMFR,
                    *reinterpret_cast<Header *>(LMFR.data() + HeaderOffset)) {}

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;
};

/// Encapsulate a database file, which:
/// - Sets/checks magic.
/// - Sets/checks version.
/// - Points at an arbitrary root table (can be changed later using a lock-free
///   algorithm).
/// - Sets up a BumpPtr for allocation.
///
/// Top-level layout:
/// - 8-bytes: Magic
/// - 8-bytes: Version
/// - 8-bytes: RootTable (16-bits: Kind; 48-bits: Offset)
/// - 8-bytes: BumpPtr
class DatabaseFile {
  static constexpr uint64_t getMagic() { return 0x00FFDA7ABA53FF00ULL; }
  static constexpr uint64_t getVersion() { return 1ULL; }
  struct Header {
    uint64_t Magic;
    uint64_t Version;
    std::atomic<int64_t> RootTableOffset;
    std::atomic<int64_t> BumpPtr;
  };

  const Header &getHeader() { return *H; }
  LazyMappedFileRegionBumpPtr &getAlloc() { return Alloc; }
  LazyMappedFileRegion &getRegion() { return Alloc.getRegion(); }

  /// Add a table.
  ///
  /// TODO: Allow lazy construction via getOrCreate()-style API.
  void addTable(TableHandle Table);

  /// Find a table. May return null.
  TableHandle findTable(StringRef Name);

  static Expected<DatabaseFile> create(LazyMappedFileRegion &LMFR,
                                       Optional<uint64_t> NewFileInitialSize);

  static Expected<DatabaseFile> get(LazyMappedFileRegion &LMFR) {
    if (Error E = validate(LMFR))
      return E;
    return DatabaseFile(LMFR);
  }
  static Expected<DatabaseFile> get(std::shared_ptr<LazyMappedFileRegion> LMFR) {
    if (Error E = validate(*LMFR))
      return E;
    return DatabaseFile(std::move(LMFR));
  }

private:
  static Error validate(LazyMappedFileRegion &LMFR);

  DatabaseFile(LazyMappedFileRegion &LMFR)
      : H(reinterpret_cast<const Header *>(LMFR.data())),
        LMFR->Alloc(LMFR, offsetof(Header, BumpPtr)) {}
  DatabaseFile(std::shared_ptr<LazyMappedFileRegion> LMFR)
      : H(reinterpret_cast<const Header *>(LMFR->data())),
        LMFR->Alloc(std::move(LMFR), offsetof(Header, BumpPtr)) {}

  const Header *H = nullptr;
  LazyMappedFileRegionBumpPtr Alloc;
};

} // end anonymous namespace

Expected<DatabaseFile> DatabaseFile::create(LazyMappedFileRegion &LMFR,
                                            Optional<uint64_t> NewFileInitialSize) {
  // Resize the underlying file to the minimum requested. Must be at least big
  // enough for the header.
  uint64_t SizeToRequest = sizeof(Header);
  if (NewFileInitialSize && *NewFileInitialSize > SizeToRequest)
    SizeToRequest = *NewFileInitialSize;
  if (Error E = LMFR.extendSize(SizeToRequest))
    return std::move(E);

  // Initialize the header and the allocator.
  new (LMFR.data()) Header{getMagic(), getVersion(), 0, 0};
  return create(LMFR);
}

void DatabaseFile::addTable(TableHandle Table) {
  assert(Table);
  assert(&Table.getRegion() == &getRegion());
  int64_t ExistingRootOffset = 0;
  const int64_t NewOffset = reinterpret_cast<const char *>(&Table.getHeader()) - LMFR.data();
  if (H->RootTableOffset.compare_exchange_strong(ExistingRootOffset, NewOffset))
    return;

  // Silently ignore attempts to set the root to itself.
  if (ExistingRootOffset == NewOffset)
    return;

  // FIXME: Fix the API so that having the same name is not an error. Instead,
  // the colliding table should just be used as-is and the client can decide
  // what to do with the new one.
  //
  // TODO: Add support for creating a chain or tree of tables (more than one at
  // all!) to avoid this error.
  TableHandle Root(*LMFR, ExistingRootOffset);
  if (Root.getName() == Table.getName())
    report_fatal_error(createStringError(std::errc::not_supported,
                                         "table name collision '" + Table.getName() + "'"));
  else
    report_fatal_error(createStringError(std::errc::not_supported,
                                         "cannot add new table '" +  Table.getName() + "'"
                                         " to existing root '" + Root.getName() + "'"));
}

TableHandle DatabaseFile::findTable(StringRef Name) {
  int64_t RootTableOffset = H->RootTableOffset.load();
  if (!RootTableOffset)
    return TableHandle();

  TableHandle Root(*LMFR, RootTableOffset);
  if (Root.getName() == Table.getName())
    return Root;

  // TODO: Once multiple tables are supported, need to walk to find them.
  return TableHandle();
}

Error DatabaseFile::validate(LazyMappedFileRegion &LMFR) {
  if (LMFR->size() < sizeof(Header))
    return createStringError(std::errc::invalid_argument, "database: missing header");

  // Check the magic and version.
  auto *H = reinterpret_cast<const Header *>(LMFR->data());
  if (H->Magic != getMagic())
    return createStringError(std::errc::invalid_argument, "database: bad magic");
  if (H->Version != getVersion())
    return createStringError(std::errc::invalid_argument, "database: wrong version");

  // Check the bump-ptr, which should be 0 or point past the header.
  if (int64_t Bump = H->BumpPtr.load())
    if (Bump < sizeof(Header))
      return createStringError(std::errc::invalid_argument, "database: corrupt bump-ptr");

  return Error::success();
}

//===----------------------------------------------------------------------===//
// HashMappedTrie data structures.
//===----------------------------------------------------------------------===//

namespace {

class SubtrieHandle;
class SubtrieSlotValue {
public:
  explicit operator bool() const { return !isEmpty(); }
  bool isEmpty() const { return !Offset; }
  bool isData() const { return Offset > 0; }
  bool isSubtrie() const { return Offset < 0; }
  int64_t asData() const {
    assert(isData());
    return Offset;
  }
  int64_t asSubtrie() const {
    assert(isSubtrie());
    return -Offset;
  }

  FileOffset asSubtrieFileOffset() const {
    return FileOffset(asSubtrie());
  }

  FileOffset asDataFileOffset() const { return FileOffset(asData()); }

  int64_t getRawOffset() const { return Offset; }

  static SubtrieSlotValue getDataOffset(int64_t Offset) {
    return SubtrieSlotValue(Offset);
  }

  static SubtrieSlotValue getSubtrieOffset(int64_t Offset) {
    return SubtrieSlotValue(-Offset);
  }

  static SubtrieSlotValue getDataOffset(FileOffset Offset) {
    return getDataOffset(Offset.get());
  }

  static SubtrieSlotValue getSubtrieOffset(FileOffset Offset) {
    return getDataOffset(Offset.get());
  }

  static SubtrieSlotValue getFromSlot(std::atomic<int64_t> &Slot) {
    return SubtrieSlotValue(Slot.load());
  }

  SubtrieSlotValue() = default;

private:
  friend class SubtrieHandle;
  explicit SubtrieSlotValue(int64_t Offset) : Offset(Offset) {}
  int64_t Offset = 0;
};

/// Subtrie layout:
/// - 2-bytes: StartBit
/// - 1-bytes: NumBits=lg(num-slots)
/// - 1-bytes: NumUnusedBits=lg(num-slots-unused)
/// - 4-bytes: 0-pad
/// - <slots>
class SubtrieHandle {
public:
  struct Header {
    /// The bit this subtrie starts on.
    uint16_t StartBit;

    /// The number of bits this subtrie handles. It has 2^NumBits slots.
    uint8_t NumBits;

    /// The number of extra bits this allocation *could* handle, due to
    /// over-allocation. It has 2^NumUnusedBits unused slots.
    uint8_t NumUnusedBits;

    /// 0-pad to 8B.
    uint32_t ZeroPad4B;
  };

  /// Slot storage:
  /// - zero:     Empty
  /// - positive: RecordOffset
  /// - negative: SubtrieOffset
  using SlotT = std::atomic<int64_t>;

  static int64_t getSlotsSize(uint32_t NumBits) {
    return sizeof(int64_t) * (1u << NumBits);
  }

  static int64_t getSize(uint32_t NumBits) {
    return sizeof(SubtrieHandle::Header) + getSlotsSize(NumBits);
  }

  int64_t getSize() const { return getSize(H->NumBits); }

  SubtrieSlotValue load(size_t I) { return SubtrieSlotValue(Slots[I].load()); }
  void store(size_t I, SubtrieSlotValue V) {
    return Slots[I].store(V.getRawOffset());
  }

  /// Return None on success, or the existing offset on failure.
  bool compare_exchange_strong(size_t I, SubtrieSlotValue &Expected,
                               SubtrieSlotValue New) {
    return Slots[I].compare_exchange_strong(Expected.Offset, New.Offset);
  }

  /// Sink \p V from \p I in this subtrie down to \p NewI in a new subtrie with
  /// \p NumSubtrieBits.
  ///
  /// \p UnusedSubtrie maintains a 1-item "free" list of unused subtries. If a
  /// new subtrie is created that isn't used because of a lost race, then it If
  /// it's already valid, it should be used instead of allocating a new one.
  /// should be returned as an out parameter to be passed back in the future.
  /// If it's already valid, it should be used instead of allocating a new one.
  ///
  /// Returns the subtrie that now lives at \p I.
  SubtrieHandle sink(size_t I, SubtrieSlotValue V,
                     LazyMappedFileRegionBumpPtr &Alloc, size_t NumSubtrieBits,
                     SubtrieHandle &UnusedSubtrie, size_t NewI);

  /// Only safe if the subtrie is empty.
  void reinitialize(uint32_t StartBit, uint32_t NumBits);

  SubtrieSlotValue getOffset() const {
    return SubtrieSlotValue::getSubtrieOffset(
        reinterpret_cast<const char *>(H) - LMFR->data());
  }

  FileOffset getFileOffset() const {
    return getOffset().asSubtrieFileOffset();
  }

  explicit operator bool() const { return H; }

  Header &getHeader() const { return *H; }
  uint32_t getStartBit() const { return H->StartBit; }
  uint32_t getNumBits() const { return H->NumBits; }
  uint32_t getNumUnusedBits() const { return H->NumUnusedBits; }

  static SubtrieHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                              uint32_t StartBit, uint32_t NumBits,
                              uint32_t NumUnusedBits = 0);

  static SubtrieHandle getFromFileOffset(LazyMappedFileRegion &LMFR, FileOffset Offset) {
    return SubtrileHandle(LMFR, SubtrieSlotValue::getSubtrieFileOffset(Offset));
  }

  SubtrieHandle() = default;
  SubtrieHandle(LazyMappedFileRegion &LMFR, Header &H)
      : LMFR(&LMFR), H(&H), Slots(getSlots(H)) {}
  SubtrieHandle(LazyMappedFileRegion &LMFR, SubtrieSlotValue Offset)
      : SubtrieHandle(LMFR, *reinterpret_cast<Header *>(LMFR.data() +
                                                        Offset.asSubtrie())) {}

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;
  MutableArrayRef<SlotT> Slots;

  static MutableArrayRef<SlotT> getSlots(Header &H) {
    return makeMutableArrayRef(reinterpret_cast<SlotT *>(&H + 1),
                               1u << H.NumBits);
  }
};

/// Handle for a HashMappedTrie table.
///
/// HashMappedTrie table layout:
/// - [8-bytes: Generic table header]
/// - 1-byte: NumSubtrieBits
/// - 1-byte:  Flags (not used yet)
/// - 2-bytes: NumHashBits
/// - 4-bytes: RecordDataSize (in bytes)
/// - 8-bytes: RootTrieOffset
/// - 8-bytes: AllocatorOffset (reserved for implementing free lists)
/// - <name> '\0'
///
/// Record layout:
/// - <data>
/// - <hash>
class HashMappedTrieHandle {
public:
  static constexpr TableHandle::TableKind Kind = TableHandle::TableKind::HashMappedTrie;

  struct Header : TableHandle::Header {
    uint8_t NumSubtrieBits;
    uint8_t Flags;          // None used yet.
    uint16_t NumHashBits;
    uint32_t RecordDataSize;
    std::atomic<int64_t> RootTrieOffset;
    std::atomic<int64_t> AllocatorOffset;
  };

  operator TableHandle() const {
    if (!H)
      return TableHandle();
    return TableHandle(*LMFR, *H);
  }

  struct RecordData {
    OnDiskHashMappedTrie::ValueProxy Proxy;
    SubtrieSlotValue Offset;
    FileOffset getFileOffset() const { return Offset.getDataFileOffset(); }
  };

  enum Limits : size_t {
    /// Seems like 65528 hash bits ought to be enough.
    MaxNumHashBytes = UINT16_MAX >> 3,
    MaxNumHashBits = MaxNumHashBytes << 3,

    /// 2^16 bits in a trie is 65536 slots. This restricts us to a 16-bit
    /// index. This many slots is suspicously large anyway.
    MaxNumRootBits = 16,

    /// 2^10 bits in a trie is 1024 slots. This many slots seems suspiciously
    /// large for subtries.
    MaxNumSubtrieBits = 10,
  };

  static constexpr size_t getNumHashBytes(size_t NumHashBits) {
    assert(NumHashBits % 8 == 0);
    return NumHashBits / 8;
  }
  static constexpr size_t getRecordSize(size_t RecordDataSize, size_t NumHashBits) {
    return RecordDataSize + getNumHashBytes(NumHashBits);
  }

  size_t getRecordDataSize() const { return H->RecordDataSize; }
  size_t getNumHashBits() const { return H->NumHashBits; }
  size_t getNumHashBytes() const { return getNumHashBytes(H->NumHashBits); }
  size_t getRecordSize() const {
    return getRecordSize(H->RecordDataSize, H->NumHashBits);
  }

  RecordData getRecord(SubtrieSlotValue Offset);
  RecordData createRecord(LazyMappedFileRegionBumpPtr &Alloc, ArrayRef<uint8_t> Hash);

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }
  SubtrieHandle getRoot() const;
  SubtrieHandle getOrCreateRoot();
  LazyMappedFileRegion &getRegion() const { return *LMFR; }

  size_t getFlags() const { return H->Flags; }
  uint64_t getNumSubtrieBits() const { return H->NumSubtrieBits; }
  uint64_t getNumHashBits() const { return H->NumHashBits; }

  IndexGenerator getIndexGen(SubtrieHandle Root, ArrayRef<uint8_t> Hash) {
    assert(Root.getStartBit() == 0);
    assert(getNumHashBits() == Hash.size() * sizeof(uint8_t));
    return IndexGenerator{Root.getNumBits(), getNumSubtrieBits(), Hash};
  }

  static HashMappedTrieHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                                     StringRef Name, Optional<uint64_t> NumRootBits,
                                     uint64_t NumSubtrieBits, uint64_t NumHashBits,
                                     uint64_t RecordDataSize);

  HashMappedTrieHandle() = default;
  HashMappedTrieHandle(LazyMappedFileRegion &LMFR, Header &H) : LMFR(&LMFR), H(&H) {}
  HashMappedTrieHandle(LazyMappedFileRegion &LMFR, intptr_t HeaderOffset)
    : HashMappedTrieHandle(LMFR,
                           *reinterpret_cast<Header *>(LMFR.data() + HeaderOffset)) {}

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;
};

struct DataFile {
  LazyMappedFileRegionBumpPtr Alloc;
  LazyMappedFileRegion &getRegion() { return Alloc.getRegion(); }

  // Path to the directory.
  //
  // FIXME: Replace with a file descriptor, and use openat APIs.
  std::string DirPath;

  DataHandle createData(ArrayRef<uint8_t> Hash, StringRef Metadata,
                        StringRef Data) {
    return DataHandle::create(Alloc, DirPath, Hash, Metadata, Data);
  }

  DataHandle getData(SubtrieSlotValue V) const {
    return DataHandle(Alloc.getRegion(), V);
  }

  OnDiskHashMappedTrie::MappedContentReference getContent(DataHandle D) const;

  static Error construct(LazyMappedFileRegion &LMFR,
                         Optional<uint64_t> NewFileInitialSize = None);

  static auto getConstructor(Optional<uint64_t> NewFileInitialSize = None) {
    return [=](LazyMappedFileRegion &LMFR) {
      return construct(LMFR, NewFileInitialSize);
    };
  }

  // FIXME: Doesn't check file magic.
  DataFile(std::shared_ptr<LazyMappedFileRegion> LMFR, std::string &&DirPath)
      : Alloc(std::move(LMFR), BumpPtrOffset), DirPath(std::move(DirPath)) {}
};

} // end anonymous namespace

struct OnDiskHashMappedTrie::ImplType {
  DatabaseFile File;
  HashMappedTrieHandle Trie;
};

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

SubtrieHandle SubtrieHandle::create(LazyMappedFileRegionBumpPtr &Alloc,
                                    uint32_t StartBit, uint32_t NumBits,
                                    uint32_t NumUnusedBits) {
  assert(StartBit <= HashMappedTrieHandle::MaxNumHashBits);
  assert(NumBits <= UINT8_MAX);
  assert(NumUnusedBits <= UINT8_MAX);
  assert(NumBits + NumUnusedBits <= HashMappedTrieHandle::MaxNumRootBits);

  int64_t Offset = Alloc.allocateOffset(getSize(NumBits + NumUnusedBits));
  auto *H = new (LMFR.data() + Offset) SubtrieHandle::Header{
      StartBit, NumBits, NumUnusedBits, /*ZeroPad4B=*/0};
  SubtrieHandle S(LMFR, *H);
  for (auto I = S.Slots.begin(), E = S.Slots.end(); I != E; ++I)
    new (I) SlotT(0);
  return S;
}

SubtrieHandle HashMappedTrieHandle::getRoot() const {
  if (int64_t Root = H->RootTrieOffset)
    return SubtrieHandle(getRegion(), SubtrieSlotValue::getSubtrieOffset(Root));
  return SubtrieHandle();
}

SubtrieHandle HashMappedTrieHandle::getOrCreateRoot() {
  if (SubtrieHandle Root = getRoot())
    return Root;

  intptr_t Race = 0;
  SubtrieSlotValue LazyRoot = SubtrieHandle::create(Alloc, 0, NumSubtrieBits);
  if (H->RootTrieOffset.compare_exchange_strong(Race, LazyRoot.asSubtrie()))
    return LazyRoot;

  // There was a race. Return the other root.
  //
  // TODO: Avoid leaking the lazy root by storing it in an allocator.
  return SubtrieHandle(getRegion(), SubtrieSlotValue::getSubtrieOffset(Race));
}

HashMappedTrieHandle HashMappedTrieHandle::create(
    LazyMappedFileRegionBumpPtr &Alloc, StringRef Name, Optional<uint64_t> NumRootBits,
    uint64_t NumSubtrieBits, uint64_t NumHashBits, uint64_t RecordDataSize) {
  // Allocate.
  intptr_t Offset = Alloc.allocateOffset(sizeof(Header) + Name.size() + 1);

  // Construct the header and the name.
  auto *H = new (Alloc.getRegion().data() + Offset) Header{
      NumSubtrieBits, /*Flags=*/0, NumHashBits,
      RecordDataSize, /*RootTrieOffset=*/0, /*AllocatorOffset=*/0};
  char *NameStorage = reinterpret_cast<char *>(H + 1);
  llvm::copy(Name, NameStorage);
  NameStorage[Name.size()] = 0;

  // Construct a root trie, if requested.
  HashMappedTrieHandle Trie(Alloc.getRegion(), *H);
  if (NumRootBits)
    H->RootTrieOffset = SubtrieHandle::create(Alloc, 0, *NumRootBits).getOffset().asSubtrie();
  return Trie;
}

HashMappedTrieHandle::RecordData
HashMappedTrieHandle::getRecord(SubtrieSlotValue Offset) {
  char *Begin = LMFR->data() + Offset.asData();
  OnDiskHashMappedTrie::ValueProxy Proxy;
  Proxy.Data = makeMutableArrayRef(Begin, getRecordDataSize());
  Proxy.Hash = makeArrayRef(reinterpret_cast<const uint8_t *>(
      Data.end(), getNumHashBytes()));
  return RecordData{Proxy, Offset};
}

HashMappedTrieHandle::RecordData
HashMappedTrieHandle::createRecord(LazyMappedFileRegionBumpPtr &Alloc,
                                   ArrayRef<uint8_t> Hash) {
  assert(&Alloc.getRegion() == LMFR);
  assert(Hash.size() == getNumHashBytes());
  RecordData Record = getRecord(SubtrieSlotValue::getDataOffset(
      Alloc.allocateOffset(getRecordSize())));
  llvm::copy(Hash, const_cast<uint8_t *>(Record.Proxy.Hash.begin()));
  return Record;
}

static Expected<LazyMappedFileRegionBumpPtr>
getAllocForNewFile(LazyMappedFileRegion &LMFR, uint64_t Magic,
                   Optional<uint64_t> NewFileInitialSize,
                   uint64_t ExpectedAllocation) {
  // Resize the underlying file to the minimum requested, or the size we expect
  // here if no minimum. Must be at least big enough for the header.
  uint64_t SizeToRequest = HeaderSize + ExpectedAllocation;
  if (NewFileInitialSize && *NewFileInitialSize > SizeToRequest)
    SizeToRequest = *NewFileInitialSize;
  if (Error E = LMFR.extendSize(SizeToRequest))
    return std::move(E);

  // Initialize the header and the allocator.
  new (LMFR.data()) uint64_t(Magic);
  new (LMFR.data() + BumpPtrOffset) std::atomic<int64_t>(0);
  return LazyMappedFileRegionBumpPtr(LMFR, BumpPtrOffset);
}

Error DataFile::construct(LazyMappedFileRegion &LMFR,
                          Optional<uint64_t> NewFileInitialSize) {
  Expected<LazyMappedFileRegionBumpPtr> Alloc = getAllocForNewFile(
      LMFR, DataMagic, NewFileInitialSize, /*ExpectedAllocation=*/0);
  if (!Alloc)
    return Alloc.takeError();

  assert(Alloc->size() == HeaderSize);
  return Error::success();
}

namespace {
/// Copy of \a sys::fs::TempFile that skips RemoveOnSignal, which is too
/// expensive to register/unregister at this rate.
///
/// FIXME: Add a TempFileManager that maintains a thread-safe list of open temp
/// files and has a signal handler registerd that removes them all.
class TempFile {
  bool Done = false;
  TempFile(StringRef Name, int FD) : TmpName(std::string(Name)), FD(FD) {}

public:
  /// This creates a temporary file with createUniqueFile and schedules it for
  /// deletion with sys::RemoveFileOnSignal.
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

  // Delete the file.
  Error discard();

  // This checks that keep or delete was called.
  ~TempFile() { assert(Done); }
};
} // namespace

Error TempFile::discard() {
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

Error TempFile::keep(const Twine &Name) {
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

Expected<TempFile> TempFile::create(const Twine &Model) {
  int FD;
  SmallString<128> ResultPath;
  if (std::error_code EC = sys::fs::createUniqueFile(Model, FD, ResultPath))
    return errorCodeToError(EC);

  TempFile Ret(ResultPath, FD);
  return std::move(Ret);
}

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

void DataHandle::createExternalContent(StringRef DirPath,
                                       ArrayRef<uint8_t> Hash,
                                       StringRef Metadata, StringRef Data) {
  assert(Data.size() >= MinExternalDataSize &&
         "Expected a bigger file for external content...");

  SmallString<256> ContentPath = DirPath;
  SmallString<128> HashString;
  appendHash(Hash, HashString);
  sys::path::append(ContentPath, HashString);

  int64_t FileSize = getDataSizeWithPadding(Data.size());

  if (sys::fs::exists(ContentPath))
    return;

  Expected<TempFile> File = TempFile::create(ContentPath + ".%%%%%%");
  if (!File)
    report_fatal_error(File.takeError());

  if (auto EC =
          sys::fs::resize_file_before_mapping_readwrite(File->FD, FileSize))
    report_fatal_error(createFileError(File->TmpName, EC));

  {
    std::error_code EC;
    sys::fs::mapped_file_region MappedFile(
        sys::fs::convertFDToNativeFile(File->FD),
        sys::fs::mapped_file_region::readwrite, FileSize, 0, EC);
    if (EC)
      report_fatal_error(createFileError(File->TmpName, EC));

    ::memcpy(MappedFile.data(), Data.begin(), Data.size());
    ::memset(MappedFile.data() + Data.size(), 0, getDataPadding(Data.size()));
  }

  if (Error E = File->keep(ContentPath))
    report_fatal_error(std::move(E));
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

SubtrieHandle SubtrieHandle::sink(size_t I, SubtrieSlotValue V,
                                  LazyMappedFileRegionBumpPtr &Alloc,
                                  size_t NumSubtrieBits,
                                  SubtrieHandle &UnusedSubtrie, size_t NewI) {
  SubtrieHandle NewS;
  if (UnusedSubtrie) {
    // Steal UnusedSubtrie and initialize it.
    std::swap(NewS, UnusedSubtrie);
    NewS.reinitialize(getStartBit() + getNumBits(), NumSubtrieBits);
  } else {
    // Allocate a new, empty subtrie.
    NewS = SubtrieHandle::create(Alloc, getStartBit() + getNumBits(), NumSubtrieBits);
  }

  NewS.store(NewI, V);
  if (compare_exchange_strong(I, V, NewS.getOffset()))
    return NewS; // Success!

  // Raced.
  assert(V.isSubtrie() && "Expected racing sink() to add a subtrie");

  // Wipe out the new slot so NewS can be reused and set the out parameter.
  NewS.store(NewI, SubtrieSlotValue());
  UnusedSubtrie = NewS;

  // Return the subtrie added by the concurrent sink() call.
  return SubtrieHandle(Alloc.getRegion(), V);
}

OnDiskHashMappedTrie::pointer
OnDiskHashMappedTrie::lookup(ArrayRef<uint8_t> Hash) {
  return mutable_pointer(const_cast<const OnDiskHashMappedTrie *>(this)->lookup(Hash));
}

OnDiskHashMappedTrie::const_pointer
OnDiskHashMappedTrie::lookup(ArrayRef<uint8_t> Hash) const {
  assert(!Hash.empty() && "Uninitialized hash");
  HashMappedTrieHandle Trie = Impl->Trie;
  SubtrieHandle S = Trie.getRoot();
  if (!S)
    return const_pointer();

  IndexGenerator IndexGen = Trie.getIndexGen(S, Hash);
  size_t Index = IndexGen.next();
  for (;;) {
    // Try to set the content.
    SubtrieSlotValue V = S.load(Index);
    if (!V)
      return const_pointer(S.getFileOffset(), HintT{this, Index, *IndexGen.StartBit});

    // Check for an exact match.
    if (V.isData()) {
      RecordData D = Trie.getRecord(V);
      return D.getHash() == Hash
                 ? const_pointer(D.getFileOffset(), D.Proxy)
                 : const_pointer(S.getFileOffset(), HintT{this, Index, *IndexGen.StartBit});
    }

    Index = IndexGen.next();
    S = SubtrieHandle(Trie.getRegion(), V);
  }
}

/// Only safe if the subtrie is empty.
void SubtrieHandle::reinitialize(uint32_t StartBit, uint32_t NumBits) {
  assert(StartBit > H->StartBit);
  assert(NumBits <= H->NumBits);
  // Ideally would also assert that all slots are empty, but that's expensive.

  H->StartBit = StartBit;
  H->NumBits = NumBits;
}

static SubtrieHandle getSubtrieFromHint(LazyMappedFileRegion &LMFR,
                                        const void *Hint) {
  auto *H = reinterpret_cast<SubtrieHandle::Header *>(const_cast<void *>(Hint));
  return SubtrieHandle(LMFR, *H);
}

OnDiskHashMappedTrie::pointer
OnDiskHashMappedTrie::insertLazy(const_pointer Hint, ArrayRef<uint8_t> Hash,
                                 LazyInsertConstructCB OnConstruct,
                                 LazyInsertLeakCB OnLeak) {
  TrieHandle Trie = Impl->Index.Trie;
  assert(Hash.size() == Trie.getNumHashBits() && "Invalid hash");

  LazyMappedFileRegionBumpPtr &Alloc = Impl->DB.getAlloc();


  SubtrieHandle S = Trie.getOrCreateRoot();
  IndexGenerator IndexGen = Trie.getIndexGen(S, Hash);

  size_t Index;
  if (Optional<HintT> H = Hint.getHint(*this)) {
    S = SubtrieHandle::getFromFileOffset(Hint.getOffset());
    Index = IndexGen.hint(H->I, H->B);
  } else {
    Index = IndexGen.next();
  }

  // FIXME: Add non-assertion based checks for data corruption that would
  // otherwise cause infinite loops in release builds, instead calling
  // report_fatal_error().
  //
  // Two loops are possible:
  // - All bits used up in the IndexGenerator because subtries are somehow
  //   linked in a cycle. Could confirm that each subtrie's start-bit
  //   follows from the start-bit and num-bits of its parent. Could also check
  //   that the generator doesn't run out of bits.
  // - Existing data matches tail of Hash but not the head (stored in an
  //   invalid spot). Probably a cheap way to check this too, but needs
  //   thought.
  Optional<RecordData> NewRecord;
  SubtrieHandle UnusedSubtrie;
  for (;;) {
    SubtrieSlotValue Existing = S.load(Index);

    // Try to set it, if it's empty.
    if (!Existing) {
      if (!NewRecord) {
        NewRecord = Trie.createRecord(Alloc, Hash);
        if (OnConstruct)
          OnConstruct(NewRecord->Offset.asDataFileOffset(), NewRecord->Proxy);
      }

      if (S.compare_exchange_strong(Index, Existing, NewRecord->Offset))
        return DataF.getContent(NewRecord);

      // Race means that Existing is no longer empty; fall through...
    }

    if (Existing.isSubtrie()) {
      S = SubtrieHandle(Trie.getRegion(), Existing);
      Index = IndexGen.next();
      continue;
    }

    // Check for an exact match.
    RecordData ExistingRecord = Trie.getRecord(Existing);
    if (ExistingRecord.Proxy.Hash == Hash) {
      if (OnLeak && NewRecord)
        OnLeak(NewRecord->Offset.asDataFileOffset(), NewRecord->Proxy,
               ExistingRecord.Offset.asDataFileOffset(), ExistingRecord.Proxy);
      return pointer(NewRecord->Offset.asDataFileOffset(),
                     NewRecord->Proxy);
    }

    // Sink the existing content as long as the indexes match.
    for (;;) {
      size_t NextIndex = IndexGen.next();
      size_t NewIndexForExistingContent =
          IndexGen.getCollidingBits(ExistingRecord.Hash);

      S = S.sink(Index, Existing, Alloc, IndexGen.getNumBits(),
                 UnusedSubtrie, NewIndexForExistingContent);
      Index = NextIndex;

      // Found the difference.
      if (NextIndex != NewIndexForExistingContent)
        break;
    }
  }
}

static Error
createTrieError(std::errc ErrC, StringRef Path, StringRef TrieName, const Twine &Msg) {
  return createStringError(Errc, Path + "[" + TrieName + "]: " + Msg);
}

static Expected<size_t>
checkParameter(StringRef Label, size_t Max, Optional<size_t> Value, Optional<size_t> Default,
               StringRef Path, StringRef TrieName) {
  assert(Value || Default);
  assert(!Default || *Default <= Max);
  if (!Value)
    return *Default;

  if (*Value <= Max)
    return *Value;
  return createTrieError(std::errc::argument_out_of_domain, Path, TrieName,
                         "invalid " + Label + ": " +
                           Twine(*Value) + " (max: " + Twine(Max) + ")");
}

static Error checkTable(StringRef Label, size_t Expected, size_t Observed,
                        StringRef Path, StringRef TrieName) {
  if (Expected == Observed)
    return Error::success();
  return createTrieError(std::errc::invalid_argument, Path, TrieName,
                         "mismatched " + Label
                           + " (expected: " + Twine(Expected)
                           + ", observed: " + Twine(Observed) + ")");
}

Expected<OnDiskHashMappedTrie>
OnDiskHashMappedTrie::create(const Twine &PathTwine, const Twine &TrieNameTwine,
                             size_t NumHashBits, uint64_t DataSize,
                             uint64_t MaxFileSize, Optional<uint64_t> NewFileInitialSize,
                             Optional<size_t> NewTableNumRootBits,
                             Optional<size_t> NewTableNumSubtrieBits) {
  std::string Path = PathTwine.str();
  SmallString<128> TrieNameStorage;
  StringRef TrieName = TrieNameTwine.toStringRef(TrieNameStorage);

  constexpr size_t DefaultNumRootBits = 10;
  constexpr size_t DefaultNumSubtrieBits = 6;

  size_t NumRootBits;
  if (Error E = checkParameter("root bits", HashMappedTrieHandle::MaxNumRootBits,
                               NewTableNumRootBits,
                               DefaultNumRootBits, Path, TrieName)
      .moveInto(NumRootBits))
    return std::move(E);

  size_t NumSubtrieBits;
  if (Error E = checkParameter("subtrie bits", HashMappedTrieHandle::MaxNumSubtrieBits,
                               NewTableNumSubtrieBits,
                               DefaultNumSubtrieBits, Path, TrieName)
      .moveInto(NumSubtrieBits))
    return std::move(E);

  size_t NumHashBytes = NumHashBits >> 3;
  if (Error E = checkParameter("hash size", HashMappedTrieHandle::MaxNumHashBits,
                               NumHashBits, None, Path, TrieName)
      .takeError())
    return std::move(E);
  assert(NumHashBits == NumHashBytes << 3 && "Expected hash size to be byte-aligned");
  if (NumHashBits != NumHashBytes << 3)
    return createTrieError(std::errc::argument_out_of_domain,
                             Path, TrieName, "invalid hash size: " +
                             Twine(*Value) + " (not byte-aligned)");

  // Constructor for if the file doesn't exist.
  auto NewFileConstructor = [&](LazyMappedFileRegion &LMFR) -> Error {
    Expected<DatabaseFile> DB = DatabaseFile::create(LMFR, NewFileInitialSize);
    if (!DB)
      return DB.takeError();

    HashMappedTrieHandle Trie =
        HashMappedTrieHandle::create(DB->getAlloc(), TrieName, NumRootBits, NumSubtrieBits,
                                     NumHashBits, DataSize);
    DB->addTable(Trie);
    return Error::success();
  };

  // Get or create the file.
  std::shared_ptr<LazyMappedFileRegion> LMFR;
  if (Error E = LazyMappedFileRegion::createShared(
                    IndexPath, MaxFileSize, NewFileConstructor)
                    .moveInto(LMFR))
    return std::move(E);

  // Find the trie and validate it.
  //
  // TODO: Add support for creating/adding a table to an existing file.
  TableHandle Table = DB->findTable(TrieName);
  if (!Table)
    return createTrieError(std::errc::argument_out_of_domain,
                             Path, TrieName, "table not found");
  if (Error E = checkTable("table kind", HashMappedTrieHandle::Kind,
                           Table.getHeader().Kind, Path, TrieName))
    return std::move(E);
  auto Trie = Table.get<HashMappedTrieHandle>();
  assert(Trie && "Already checked the kind");

  // Check the hash and data size.
  if (Error E = checkTable("hash size", NumHashBits,
                           Trie.getNumHashBits(), Path, TrieName))
    return std::move(E);
  if (Error E = checkTable("data size", DataSize,
                           Trie.getRecordDataSize(), Path, TrieName))
    return std::move(E);

  // No flags supported right now. Either corrupt, or coming from a future
  // writer.
  if (size_t Flags = Trie.getFlags())
    return createTrieError(std::errc::invalid_argument,
                           Path, TrieName, "unsupported flags: " + Twine(Flags));

  // Success.
  OnDiskHashMappedTrie::ImplType Impl{DatabaseFile(std::move(*DB)), Trie};
  return OnDiskHashMappedTrie(std::make_unique<ImplType>(std::move(Impl)));
}

OnDiskHashMappedTrie::OnDiskHashMappedTrie(std::unique_ptr<ImplType> Impl)
    : Impl(std::move(Impl)) {}
OnDiskHashMappedTrie::OnDiskHashMappedTrie(OnDiskHashMappedTrie &&RHS) =
    default;
OnDiskHashMappedTrie &
OnDiskHashMappedTrie::operator=(OnDiskHashMappedTrie &&RHS) = default;
OnDiskHashMappedTrie::~OnDiskHashMappedTrie() = default;

//===----------------------------------------------------------------------===//
// DataSink data structures.
//===----------------------------------------------------------------------===//

namespace {
/// DataSink table layout:
/// - [8-bytes: Generic table header]
/// - 8-bytes: AllocatorOffset (reserved for implementing free lists)
///
/// Record layout:
/// - <data>
class DataSink {
public:
  static constexpr TableHandle::TableKind Kind = TableHandle::TableKind::DataSink;

  struct Header : TableHandle::Header {
    std::atomic<int64_t> AllocatorOffset;
  };

  operator TableHandle() const {
    if (!H)
      return TableHandle();
    return TableHandle(*LMFR, *H);
  }

  using RecordData = OnDiskDataSink::ValueProxy;

  RecordData createRecord(LazyMappedFileRegionBumpPtr &Alloc, size_t DataSize) {
    assert(Alloc.getRegion() == LMFR);
    return RecordData{makeMutableArrayRef(Alloc.allocate(DataSize), DataSize),
                      FileOffset(Begin - LMFR->data())};
  }

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }
  LazyMappedFileRegion &getRegion() const { return *LMFR; }

  static DataSinkHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                               StringRef Name);

  DataSinkHandle() = default;
  DataSinkHandle(LazyMappedFileRegion &LMFR, Header &H) : LMFR(&LMFR), H(&H) {}
  DataSinkHandle(LazyMappedFileRegion &LMFR, intptr_t HeaderOffset)
      : DataSinkHandle(LMFR,
                       *reinterpret_cast<Header *>(LMFR.data() + HeaderOffset)) {}

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;
};

} // end anonymous namespace

struct OnDiskDataSink::ImplType {
  DatabaseFile File;
  DataSink Store;
};

RecordData DataSinkHandle::createRecord(LazyMappedFileRegionBumpPtr &Alloc, size_t DataSize) {
  // Allocate.
  assert(Alloc.getRegion() == LMFR);
  char *Begin = Alloc.allocate(getRecordSize(DataSize));

  // Initialize the padding to 0 for consistency.
  char *Padding = Begin + DataSize;
  ::memset(MutableHash.end(), 0, getPaddingSize());

  return RecordData{makeMutableArrayRef(Begin, DataSize),
    FileOffset(Begin - LMFR->data())};
}

OnDiskDataSink::OnDiskDataSink(std::unique_ptr<ImplType> Impl)
    : Impl(std::move(Impl)) {}
OnDiskDataSink::OnDiskDataSink(OnDiskDataSink &&RHS) = default;
OnDiskDataSink &OnDiskDataSink::operator=(OnDiskDataSink &&RHS) = default;
OnDiskDataSink::~OnDiskDataSink() = default;

FileOffset OnDiskDataSink::getFileOffset(const_pointer P) const {
  assert(Impl);
  assert(P);
  assert(P->begin());
  uintptr_t Data = reinterpret_cast<uintptr_t>(Impl->File.getRegion().data());
  uintptr_t DataEnd = Data + Impl->File.getAlloc().size();
  assert(DataEnd - Data <= (uintptr_t)INTPTR_MAX);

  uintptr_t Begin = reinterpret_cast<uintptr_t>(P->begin());
  assert(Begin >= Data + sizeof(DatabaseFile::Header));
  assert(Begin < DataEnd);
  return FileOffset((intptr_t)(Data - Begin));
}

char *OnDiskDataSink::beginData(FileOffset Offset) {
}

const char *OnDiskDataSink::beginData(FileOffset Offset) const {
  assert(Offset);
  assert(Impl);
  assert(Offset.get() < Impl->File.getAlloc().size());
  return Impl->File.getRegion().data() + Offset.get();
}

/// TODO: Incorporate into BuiltinCAS.
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

  static void createExternalContent(StringRef DirPath, ArrayRef<uint8_t> Hash,
                                    StringRef Metadata, StringRef Data);
};


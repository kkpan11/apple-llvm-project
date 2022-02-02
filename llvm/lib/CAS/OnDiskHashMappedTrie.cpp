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

  int64_t getRawOffset() const { return Offset; }

  static SubtrieSlotValue getDataOffset(int64_t Offset) {
    return SubtrieSlotValue(Offset);
  }

  static SubtrieSlotValue getSubtrieOffset(int64_t Offset) {
    return SubtrieSlotValue(-Offset);
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

struct OnDiskIndexHeader;
class SubtrieHandle {
public:
  struct Header {
    uint32_t StartBit;
    uint32_t NumBits;
  };

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
  Optional<SubtrieSlotValue> try_set(size_t I, SubtrieSlotValue New);

  /// Return None on success, or the existing offset on failure.
  Optional<SubtrieSlotValue> try_replace(size_t I, SubtrieSlotValue Expected,
                                         SubtrieSlotValue New);

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

  explicit operator bool() const { return H; }

  Header &getHeader() const { return *H; }
  uint32_t getStartBit() const { return H->StartBit; }
  uint32_t getNumBits() const { return H->NumBits; }

  static SubtrieHandle create(LazyMappedFileRegionBumpPtr &Alloc,
                              uint32_t StartBit, uint32_t NumBits);

  SubtrieHandle() = default;
  SubtrieHandle(LazyMappedFileRegion &LMFR, Header &H)
      : LMFR(&LMFR), H(&H), Slots(getSlots(H)) {}
  SubtrieHandle(LazyMappedFileRegion &LMFR, SubtrieSlotValue Offset)
      : SubtrieHandle(LMFR, *reinterpret_cast<Header *>(LMFR.data() +
                                                        Offset.asSubtrie())) {}

private:
  LazyMappedFileRegion *LMFR = nullptr;
  Header *H = nullptr;
  MutableArrayRef<std::atomic<int64_t>> Slots;

  static MutableArrayRef<std::atomic<int64_t>> getSlots(Header &H) {
    return makeMutableArrayRef(reinterpret_cast<std::atomic<int64_t> *>(&H + 1),
                               1u << H.NumBits);
  }
};

struct OnDiskData final {
  unsigned HashSize : 16;
  unsigned HashPaddingSize : 8;
  unsigned MetadataPaddingSize : 8;
  unsigned MetadataSize : 31;
  unsigned IsEmbedded : 1;
  uint64_t DataSize;

  // Files 64KB and bigger can just be saved separately.
  //
  // FIXME: Should be configurable.
  static constexpr int64_t MaxEmbeddedDataSize = 64ull * 1024ull - 1ull;

  // FIXME: This is being used as a hack to disable an assertion. The problem
  // is that if there are LOTS of references it the number above can exceed
  // MaxEmbeddedDataSize, even if the data itself is tiny. The right fix is to
  // sometimes (always?) make references external too.
  static constexpr int64_t MinExternalDataSize = 4ull * 1024ull - 1ull;

  static uint64_t getHashOffset() { return sizeof(OnDiskData); }

  static uint64_t getPadding(uint64_t Size) {
    if (uint64_t Remainder = Size % 8)
      return 8 - Remainder;
    return 0;
  }

  static uint64_t getHashPaddingStart(uint64_t HashSize) {
    return sizeof(OnDiskData) + HashSize;
  }

  static uint64_t getSizeIfExternal(uint64_t HashSize, uint64_t MetadataSize) {
    return sizeof(OnDiskData) + HashSize + getPadding(HashSize) + MetadataSize +
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
    return makeArrayRef(
        reinterpret_cast<const uint8_t *>(this) + getHashOffset(), HashSize);
  }

  bool isEmbedded() const { return IsEmbedded; }
  bool isExternal() const { return !isEmbedded(); }

  const char *getHashStart() const {
    return reinterpret_cast<const char *>(this) + sizeof(OnDiskData);
  }
  const char *getHashPaddingStart() const { return getHashStart() + HashSize; }
  const char *getMetadataStart() const {
    return getHashPaddingStart() + HashPaddingSize;
  }
  const char *getMetadataPaddingStart() const {
    return getMetadataStart() + MetadataSize;
  }
  const char *getDataStart() const {
    return getMetadataPaddingStart() + MetadataPaddingSize;
  }
  const char *getDataPaddingStart() const { return getDataStart() + DataSize; }
  StringRef getMetadata() const {
    return StringRef(getMetadataStart(), MetadataSize);
  }

  Optional<StringRef> getData() const {
    if (isExternal())
      return None;
    return StringRef(getDataStart(), DataSize);
  }

  using MappedContentReference = OnDiskHashMappedTrie::MappedContentReference;
  MappedContentReference getContent(StringRef DirPath) const;

  static void createExternalContent(StringRef DirPath, ArrayRef<uint8_t> Hash,
                                    StringRef Metadata, StringRef Data);

private:
  friend struct OnDiskDataHeader;
  struct ImplTag {};
  struct ExternalTag {};
  OnDiskData(ImplTag, bool IsEmbedded, ArrayRef<uint8_t> Hash,
             StringRef Metadata, uint64_t DataSize);
  OnDiskData(ExternalTag, ArrayRef<uint8_t> Hash, StringRef Metadata,
             uint64_t DataSize);
  OnDiskData(ArrayRef<uint8_t> Hash, StringRef Metadata, StringRef Data);
};

/// Some magic numbers.
///
/// FIXME: This magic is really horrible. Do better.
enum MagicNumbers : uint64_t {
  IndexMagic = 12345678ULL,
  DataMagic = 87654321ULL,
};

/// A few sizes that matter...
enum CommonSizes : uint64_t {
  MagicSize = sizeof(uint64_t),
  HeaderSize = 16,
};
static_assert(HeaderSize == MagicSize + sizeof(std::atomic<int64_t>),
              "Math not working out...");

struct OnDiskIndexHeader {
  uint64_t NumRootBits;
  uint64_t NumSubtrieBits;
  uint64_t NumHashBits;
  SubtrieHandle::Header Root;

  OnDiskIndexHeader(uint64_t NumRootBits, uint64_t NumSubtrieBits,
                    uint64_t NumHashBits)
      : NumRootBits(NumRootBits), NumSubtrieBits(NumSubtrieBits),
        NumHashBits(NumHashBits), Root{0, (uint32_t)NumRootBits} {}
};

struct OnDiskDataHeader {
  OnDiskData *getData(SubtrieSlotValue Slot) {
    assert(Slot.isData());
    assert(Slot.asData() >= int64_t(HeaderSize));
    return reinterpret_cast<OnDiskData *>(reinterpret_cast<char *>(this) +
                                          Slot.asData() - HeaderSize);
  }
  SubtrieSlotValue createData(StringRef DirPath,
                              LazyMappedFileRegionBumpPtr &Alloc,
                              ArrayRef<uint8_t> Hash, StringRef Metadata,
                              StringRef Data);
};
} // namespace

struct OnDiskHashMappedTrie::ImplType {
  // Path to the directory.
  //
  // FIXME: Replace with a file descriptor, and use openat APIs.
  std::string DirPath;

  // The index is where the trie actually lives.
  struct {
    OnDiskIndexHeader *Header = nullptr;
    LazyMappedFileRegionBumpPtr Alloc;

    LazyMappedFileRegion &getRegion() { return Alloc.getRegion(); }
  } Index;

  // This is storage for data; not really part of the trie.
  //
  // FIXME: Separate this out of the on-disk trie data structure.
  struct {
    OnDiskDataHeader *Header = nullptr;
    LazyMappedFileRegionBumpPtr Alloc;

    LazyMappedFileRegion &getRegion() { return Alloc.getRegion(); }
  } Data;
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

OnDiskData::OnDiskData(ImplTag, bool IsEmbedded, ArrayRef<uint8_t> Hash,
                       StringRef Metadata, uint64_t DataSize)
    : HashSize(Hash.size()), HashPaddingSize(getPadding(Hash.size())),
      MetadataPaddingSize(getPadding(Metadata.size())),
      MetadataSize(Metadata.size()), IsEmbedded(IsEmbedded),
      DataSize(DataSize) {
  assert(Metadata.size() < (1u << 31) && "Too much metadata");
  ::memcpy(const_cast<char *>(getHashStart()), Hash.begin(), HashSize);
  ::memcpy(const_cast<char *>(getMetadataStart()), Metadata.begin(),
           MetadataSize);

  // Fill padding with 0s in case the filesystem doesn't guarantee it
  // (Windows).
  ::memset(const_cast<char *>(getHashPaddingStart()), 0, HashPaddingSize);
  ::memset(const_cast<char *>(getMetadataPaddingStart()), 0,
           MetadataPaddingSize);
}

OnDiskData::OnDiskData(ExternalTag, ArrayRef<uint8_t> Hash, StringRef Metadata,
                       uint64_t DataSize)
    : OnDiskData(ImplTag(), /*IsEmbedded=*/false, Hash, Metadata, DataSize) {}

OnDiskData::OnDiskData(ArrayRef<uint8_t> Hash, StringRef Metadata,
                       StringRef Data)
    : OnDiskData(ImplTag(), /*IsEmbedded=*/true, Hash, Metadata, Data.size()) {
  ::memcpy(const_cast<char *>(getDataStart()), Data.begin(), DataSize);

  // Fill padding with 0s in case the filesystem doesn't guarantee it
  // (Windows).
  ::memset(const_cast<char *>(getDataPaddingStart()), 0,
           getDataPadding(DataSize));
}

SubtrieHandle SubtrieHandle::create(LazyMappedFileRegionBumpPtr &Alloc,
                                    uint32_t StartBit, uint32_t NumBits) {
  int64_t Offset = Alloc.allocateOffset(getSize(NumBits));
  char *Mem = Alloc.data() + Offset;
  auto *H = new (Mem) SubtrieHandle::Header{StartBit, NumBits};
  SubtrieHandle S(Alloc.getRegion(), *H);
  for (auto I = S.Slots.begin(), E = S.Slots.end(); I != E; ++I)
    new (I) std::atomic<int64_t>(0);
  return S;
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
OnDiskData::getContent(StringRef DirPath) const {
  if (isEmbedded())
    return MappedContentReference(getMetadata(), *getData());

  SmallString<128> HashString;
  appendHash(getHash(), HashString);
  SmallString<256> ContentPath = DirPath;
  sys::path::append(ContentPath, HashString);

  Expected<sys::fs::file_t> ContentFD =
      sys::fs::openNativeFileForRead(ContentPath, sys::fs::OF_None);
  if (!ContentFD)
    report_fatal_error(ContentFD.takeError());
  auto CloseOnReturn =
      llvm::make_scope_exit([&ContentFD]() { sys::fs::closeFile(*ContentFD); });

  // Load with the padding to guarantee null termination.
  int64_t ExternalSize = getDataSizeWithPadding(DataSize);
  sys::fs::mapped_file_region Map;
  {
    std::error_code EC;
    Map = sys::fs::mapped_file_region(
        sys::fs::convertFDToNativeFile(*ContentFD),
        sys::fs::mapped_file_region::readonly, ExternalSize, 0, EC);
    if (EC)
      report_fatal_error(createFileError(ContentPath, EC));
  }

  StringRef Data(Map.data(), DataSize);
  return MappedContentReference(getMetadata(), Data, std::move(Map));
}

void OnDiskData::createExternalContent(StringRef DirPath,
                                       ArrayRef<uint8_t> Hash,
                                       StringRef Metadata, StringRef Data) {
  assert(Data.size() >= MinExternalDataSize &&
         "Expected a bigger file for external content...");

  SmallString<256> ContentPath = DirPath;
  SmallString<128> HashString;
  appendHash(Hash, HashString);
  sys::path::append(ContentPath, HashString);

  int64_t FileSize = OnDiskData::getDataSizeWithPadding(Data.size());

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
    ::memset(MappedFile.data() + Data.size(), 0,
             OnDiskData::getDataPadding(Data.size()));
  }

  if (Error E = File->keep(ContentPath))
    report_fatal_error(std::move(E));
}

SubtrieSlotValue OnDiskDataHeader::createData(
    StringRef DirPath, LazyMappedFileRegionBumpPtr &Alloc,
    ArrayRef<uint8_t> Hash, StringRef Metadata, StringRef Data) {
  int64_t Size = OnDiskData::getSizeIfEmbedded(Hash, Metadata, Data);

  // FIXME: This logic isn't right. References should be stored externally if
  // there are enough that we'll go over the max embedded data size limit.
  // Right now only the data is ever made external.... but we could blow out
  // the main mmap if there are lots of references.
  if (Size <= OnDiskData::MaxEmbeddedDataSize ||
      Data.size() < OnDiskData::MinExternalDataSize) {
    auto Offset = SubtrieSlotValue::getDataOffset(Alloc.allocateOffset(Size));
    new (getData(Offset)) OnDiskData(Hash, Metadata, Data);
    return Offset;
  }

  OnDiskData::createExternalContent(DirPath, Hash, Metadata, Data);
  Size = OnDiskData::getSizeIfExternal(Hash.size(), Metadata.size());
  auto Offset = SubtrieSlotValue::getDataOffset(Alloc.allocateOffset(Size));
  new (getData(Offset))
      OnDiskData(OnDiskData::ExternalTag(), Hash, Metadata, Data.size());
  return Offset;
}

Optional<SubtrieSlotValue> SubtrieHandle::try_set(size_t I,
                                                  SubtrieSlotValue New) {
  return try_replace(I, SubtrieSlotValue(), New);
}

Optional<SubtrieSlotValue> SubtrieHandle::try_replace(size_t I,
                                                      SubtrieSlotValue Expected,
                                                      SubtrieSlotValue New) {
  assert(New);
  int64_t Old = Expected.Offset;
  if (!Slots[I].compare_exchange_strong(Old, New.Offset)) {
    assert(Old != Expected.Offset);
    return SubtrieSlotValue(Old);
  }
  // Success.
  return None;
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
  Optional<SubtrieSlotValue> Race = try_replace(I, V, NewS.getOffset());
  if (!Race)
    return NewS; // Success!
  assert(Race->isSubtrie());

  // Wipe out the new slot so NewS can be reused and set the out parameter.
  NewS.store(NewI, SubtrieSlotValue());
  UnusedSubtrie = NewS;

  // Return the subtrie added by the concurrent sink() call.
  return SubtrieHandle(Alloc.getRegion(), *Race);
}

OnDiskHashMappedTrie::LookupResult
OnDiskHashMappedTrie::lookup(ArrayRef<uint8_t> Hash) const {
  assert(!Hash.empty() && "Uninitialized hash");
  auto *IndexHeader = Impl->Index.Header;
  auto *DataHeader = Impl->Data.Header;
  assert(IndexHeader->NumHashBits == Hash.size() * sizeof(uint8_t));

  SubtrieHandle S(Impl->Index.getRegion(), IndexHeader->Root);
  IndexGenerator IndexGen{IndexHeader->NumRootBits, IndexHeader->NumSubtrieBits,
                          Hash};
  size_t Index = IndexGen.next();
  for (;;) {
    // Try to set the content.
    SubtrieSlotValue Existing = S.load(Index);
    if (!Existing)
      return LookupResult(&S.getHeader(), Index, *IndexGen.StartBit);

    // Check for an exact match.
    if (Existing.isData()) {
      // FIXME: Stop calling Data->getPath() here; pass in parent directory.
      OnDiskData *ExistingData = DataHeader->getData(Existing);
      return ExistingData->getHash() == Hash
                 ? LookupResult(ExistingData->getContent(Impl->DirPath))
                 : LookupResult(&S.getHeader(), Index, *IndexGen.StartBit);
    }

    Index = IndexGen.next();
    S = SubtrieHandle(Impl->Index.getRegion(), Existing);
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

OnDiskHashMappedTrie::MappedContentReference
OnDiskHashMappedTrie::insert(LookupResult Hint, ArrayRef<uint8_t> Hash,
                             StringRef Metadata, StringRef Content) {
  assert(!Hash.empty() && "Uninitialized hash");
  auto *IndexHeader = Impl->Index.Header;
  auto *DataHeader = Impl->Data.Header;
  assert(IndexHeader->NumHashBits == Hash.size() * sizeof(uint8_t));

  SubtrieHandle S(Impl->Index.getRegion(), IndexHeader->Root);
  IndexGenerator IndexGen{IndexHeader->NumRootBits, IndexHeader->NumSubtrieBits,
                          Hash};
  size_t Index;
  if (Hint.isHint()) {
    S = getSubtrieFromHint(Impl->Index.getRegion(), Hint.S);
    Index = IndexGen.hint(Hint.I, Hint.B);
  } else {
    Index = IndexGen.next();
  }

  Optional<SubtrieSlotValue> NewData;
  SubtrieHandle UnusedSubtrie;
  for (;;) {
    // To minimize leaks, first check the data, only allocating an on-disk
    // record if it's not already in the map.
    SubtrieSlotValue Existing = S.load(Index);

    // Try to set it, if it's empty.
    if (!Existing) {
      if (!NewData)
        NewData = DataHeader->createData(Impl->DirPath, Impl->Data.Alloc, Hash,
                                         Metadata, Content);
      assert(NewData->asData() < int64_t(Impl->Data.Alloc.size()));

      // FIXME: Stop calling Data->getPath() here; pass in parent directory.
      Optional<SubtrieSlotValue> Race = S.try_set(Index, *NewData);
      if (!Race) // Success!
        return DataHeader->getData(*NewData)->getContent(Impl->DirPath);
      assert(*Race && "Expected non-empty, or else why the race?");
      Existing = *Race;
    }

    if (Existing.isSubtrie()) {
      S = SubtrieHandle(Impl->Index.getRegion(), Existing);
      Index = IndexGen.next();
      continue;
    }

    // Check for an exact match.
    //
    // FIXME: Stop calling Data->getPath() here; pass in parent directory.
    OnDiskData *ExistingData = DataHeader->getData(Existing);
    if (ExistingData->getHash() == Hash)
      return ExistingData->getContent(Impl->DirPath); // Already there!

    // Sink the existing content as long as the indexes match.
    for (;;) {
      size_t NextIndex = IndexGen.next();
      size_t NewIndexForExistingContent =
          IndexGen.getCollidingBits(ExistingData->getHash());

      S = S.sink(Index, Existing, Impl->Index.Alloc, IndexGen.getNumBits(),
                 UnusedSubtrie, NewIndexForExistingContent);
      Index = NextIndex;

      // Found the difference.
      if (NextIndex != NewIndexForExistingContent)
        break;
    }
  }
}

Expected<OnDiskHashMappedTrie>
OnDiskHashMappedTrie::create(const Twine &PathTwine, size_t NumHashBits,
                             uint64_t MaxMapSize,
                             Optional<size_t> InitialNumRootBits,
                             Optional<size_t> InitialNumSubtrieBits) {
  static constexpr size_t DefaultNumRootBits = 10;
  static constexpr size_t DefaultNumSubtrieBits = 6;

  if (!InitialNumRootBits)
    InitialNumRootBits = DefaultNumRootBits;
  if (!InitialNumSubtrieBits)
    InitialNumSubtrieBits = DefaultNumSubtrieBits;

  SmallString<128> Path, DataPath, IndexPath;
  PathTwine.toVector(Path);
  DataPath = Path;
  IndexPath = Path;
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

  constexpr uint64_t BumpPtrOffset = MagicSize;
  assert(HeaderSize == BumpPtrOffset + sizeof(std::atomic<int64_t>));

  constexpr int64_t DataEndOffset = HeaderSize;
  const int64_t IndexEndOffset =
      HeaderSize + sizeof(OnDiskIndexHeader) +
      SubtrieHandle::getSlotsSize(*InitialNumRootBits);

  // Open / create / initialize files on disk.
  std::shared_ptr<LazyMappedFileRegion> IndexRegion, DataRegion;
  if (Error E =
          LazyMappedFileRegion::createShared(
              IndexPath, MaxIndexSize, /*NewFileSize=*/MB,
              [&](char *FileData) {
                new (FileData) uint64_t(IndexMagic);
                new (FileData + BumpPtrOffset) std::atomic<int64_t>(IndexEndOffset);
                new (FileData + HeaderSize) OnDiskIndexHeader(
                    *InitialNumRootBits, *InitialNumSubtrieBits, NumHashBits);
                return Error::success();
              })
              .moveInto(IndexRegion))
    return std::move(E);
  if (Error E = LazyMappedFileRegion::createShared(
                    DataPath, MaxDataSize, /*NewFileSize=*/MB,
                    [&](char *FileData) {
                      new (FileData) uint64_t(DataMagic);
                      new (FileData + BumpPtrOffset)
                          std::atomic<int64_t>(DataEndOffset);
                      return Error::success();
                    })
                    .moveInto(DataRegion))
    return std::move(E);

  // FIXME: The magic should be checked...

  // Success.
  auto *Index = reinterpret_cast<OnDiskIndexHeader *>(IndexRegion->data() + HeaderSize);
  auto *Data = reinterpret_cast<OnDiskDataHeader *>(DataRegion->data() + HeaderSize);
  OnDiskHashMappedTrie::ImplType Impl = {
      Path.str().str(),
      {Index,
       LazyMappedFileRegionBumpPtr(std::move(IndexRegion), BumpPtrOffset)},
      {Data,
       LazyMappedFileRegionBumpPtr(std::move(DataRegion), BumpPtrOffset)}};
  return OnDiskHashMappedTrie(std::make_unique<ImplType>(std::move(Impl)));
}

OnDiskHashMappedTrie::OnDiskHashMappedTrie(std::unique_ptr<ImplType> Impl)
    : Impl(std::move(Impl)) {}
OnDiskHashMappedTrie::OnDiskHashMappedTrie(OnDiskHashMappedTrie &&RHS) =
    default;
OnDiskHashMappedTrie &
OnDiskHashMappedTrie::operator=(OnDiskHashMappedTrie &&RHS) = default;
OnDiskHashMappedTrie::~OnDiskHashMappedTrie() = default;

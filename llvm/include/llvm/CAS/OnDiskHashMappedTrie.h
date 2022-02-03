//===- OnDiskHashMappedTrie.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_ONDISKHASHMAPPEDTRIE_H
#define LLVM_CAS_ONDISKHASHMAPPEDTRIE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA1.h"
#include <atomic>
#include <mutex>

namespace llvm {

class MemoryBuffer;

namespace cas {

class FileOffset {
public:
  int64_t get() const { return Offset; }

  explicit operator bool() const { return Offset; }

  FileOffset() = default;
  explicit FileOffset(int64_t Offset) : Offset(Offset) {
    assert(Offset >= 0);
  }

private:
  int64_t Offset = 0;
};

/// On-disk hash-mapped trie. Thread-safe / lock-free.
///
/// This is an on-disk, (mostly) thread-safe key-value store that is (mostly)
/// lock-free. The keys are fixed length, and are expected to be binary hashes
/// with a normal distribution.
///
/// - Thread-safety is achieved through the use of atomics within a shared
///   memory mapping. Atomic access does not work on networked filesystems.
/// - Filesystem locks are used, but only sparingly:
///     - during initialization, for creating / opening an existing store;
///     - rarely on insertion, to resize the store (see \a
///       OnDiskHashMappedTrie::MappedFileInfo::requestFileSize()).
/// - Path is used as a directory:
///     - "index" stores the root trie and subtries.
///     - "data" stores (most of) the entries, like a bump-ptr-allocator.
///     - Large entries are stored externally in a file named by the key.
/// - Code is system-dependent (Windows not yet implemented), and binary format
///   itself is not portable. These are not artifacts that can/should be moved
///   between different systems; they are only appropriate for local storage.
///
/// FIXME: Add support for storing top-level metadata or identifiers that can
/// be created / read during initialization.
///
/// FIXME: Implement for Windows. See comment next to implementation of \a
/// OnDiskHashMappedTrie::MappedFileInfo::open().
///
/// HashMappedTrie table layout:
/// - 16-bytes: Generic table header
/// - 8-bytes: HashMappedTrieVersion
/// - 2-bytes: NumRootBits
/// - 2-bytes: NumSubtrieBits
/// - 2-bytes: NumHashBits
/// - 2-bytes: RecordDataSize (in bytes)
/// - 8-bytes: AllocatorOffset (reserved for implementing free lists)
///
/// Subtrie layout:
/// - 2-bytes: TableKind
/// - 2-bytes: HashMappedTrieVersion & 0xffff
/// - 2-bytes: StartBit
/// - 2-bytes: NumBits
/// - <slots>: +ve: RecordOffset
///            -ve: SubtrieOffset
///            0:   Empty
///
/// Record layout:
/// - <hash>
/// - {0-7}-bytes: 0-pad to 8B
/// - <data>
/// - {0-7}-bytes: 0-pad to 8B
class OnDiskHashMappedTrie {
public:
  LLVM_DUMP_METHOD void dump() const;
  void print(raw_ostream &OS) const;

public:
  struct MappedContentReference {
    StringRef Metadata;              // Not null-terminated.
    StringRef Data;                  // Always used, always null-terminated.
    sys::fs::mapped_file_region Map; // Needed for large blobs.

    MappedContentReference() = delete;

    explicit MappedContentReference(
        StringRef Metadata, StringRef Data,
        sys::fs::mapped_file_region Map = sys::fs::mapped_file_region())
        : Metadata(Metadata), Data(Data), Map(std::move(Map)) {}
    MappedContentReference(MappedContentReference &&) = default;
    MappedContentReference &operator=(MappedContentReference &&) = default;
  };

  /// Result of a lookup. Suitable for an insertion hint. Maybe could be
  /// expanded into an iterator of sorts, but likely not useful (visiting
  /// everything in the trie should probably be done some way other than
  /// through an iterator pattern).
  class LookupResult {
  public:
    MappedContentReference &&take() {
      assert(Content && "Expected valid content");
      return std::move(*Content);
    }

    /// Returns true if \a get() is not \c None.
    explicit operator bool() const { return I == -2u; }

    LookupResult() = default;
    LookupResult(LookupResult &&) = default;
    LookupResult &operator=(LookupResult &&) = default;

    LookupResult(const LookupResult &) = delete;
    LookupResult &operator=(const LookupResult &) = delete;

  private:
    friend class OnDiskHashMappedTrie;
    LookupResult(MappedContentReference Content)
        : Content(std::move(Content)), I(-2u) {}
    LookupResult(void *S, unsigned I, unsigned B) : S(S), I(I), B(B) {}

    bool isHint() const { return I != -1u && I != -2u; }

    Optional<MappedContentReference> Content;
    const void *S = nullptr;
    unsigned I = -1u;
    unsigned B = 0;
  };

  LookupResult lookup(ArrayRef<uint8_t> Hash) const;

  /// Returns the content in the map.
  MappedContentReference insert(LookupResult Hint, ArrayRef<uint8_t> Hash,
                                StringRef Metadata, StringRef Data);

  MappedContentReference insert(ArrayRef<uint8_t> Hash, StringRef Metadata,
                                StringRef Data) {
    return insert(LookupResult(), Hash, Metadata, Data);
  }

  static Expected<OnDiskHashMappedTrie>
  create(const Twine &Path, size_t NumHashBits, uint64_t MaxMapSize,
         Optional<size_t> InitialNumRootBits = None,
         Optional<size_t> InitialNumSubtrieBits = None);

  OnDiskHashMappedTrie(OnDiskHashMappedTrie &&RHS);
  OnDiskHashMappedTrie &operator=(OnDiskHashMappedTrie &&RHS);

  // No copy. Just call \a create() again.
  OnDiskHashMappedTrie(const OnDiskHashMappedTrie &) = delete;
  OnDiskHashMappedTrie &operator=(const OnDiskHashMappedTrie &) = delete;

  ~OnDiskHashMappedTrie();

private:
  struct ImplType;
  explicit OnDiskHashMappedTrie(std::unique_ptr<ImplType> Impl);
  std::unique_ptr<ImplType> Impl;
};

/// Storage for data.
///
/// DataStore table layout:
/// - [16-bytes: Generic table header]
/// - 8-bytes: DataStoreVersion
/// - 8-bytes: AllocatorOffset (reserved for implementing free lists)
///
/// Record layout:
/// - <data>
/// - {0..7}-bytes: 0-pad to 8B
class OnDiskDataStore {
public:
  struct MappedContentReference {
    StringRef Data;                  // Always used, always null-terminated.
    sys::fs::mapped_file_region Map; // Needed for large blobs.

    MappedContentReference() = delete;

    explicit MappedContentReference(
        StringRef Metadata, StringRef Data,
        sys::fs::mapped_file_region Map = sys::fs::mapped_file_region())
        : Metadata(Metadata), Data(Data), Map(std::move(Map)) {}
    MappedContentReference(MappedContentReference &&) = default;
    MappedContentReference &operator=(MappedContentReference &&) = default;
  };

  /// Result of a lookup. Suitable for an insertion hint. Maybe could be
  /// expanded into an iterator of sorts, but likely not useful (visiting
  /// everything in the trie should probably be done some way other than
  /// through an iterator pattern).
  class LookupResult {
  public:
    MappedContentReference &&take() {
      assert(Content && "Expected valid content");
      return std::move(*Content);
    }

    /// Returns true if \a get() is not \c None.
    explicit operator bool() const { return I == -2u; }

    LookupResult() = default;
    LookupResult(LookupResult &&) = default;
    LookupResult &operator=(LookupResult &&) = default;

    LookupResult(const LookupResult &) = delete;
    LookupResult &operator=(const LookupResult &) = delete;

  private:
    friend class OnDiskHashMappedTrie;
    LookupResult(MappedContentReference Content)
        : Content(std::move(Content)), I(-2u) {}
    LookupResult(void *S, unsigned I, unsigned B) : S(S), I(I), B(B) {}

    bool isHint() const { return I != -1u && I != -2u; }

    Optional<MappedContentReference> Content;
    const void *S = nullptr;
    unsigned I = -1u;
    unsigned B = 0;
  };

  LookupResult lookup(ArrayRef<uint8_t> Hash) const;

  /// Returns the content in the map.
  MappedContentReference insert(LookupResult Hint, ArrayRef<uint8_t> Hash,
                                StringRef Metadata, StringRef Data);

  MappedContentReference insert(ArrayRef<uint8_t> Hash, StringRef Metadata,
                                StringRef Data) {
    return insert(LookupResult(), Hash, Metadata, Data);
  }

  static Expected<OnDiskHashMappedTrie>
  create(const Twine &Path, size_t NumHashBits, uint64_t MaxMapSize,
         Optional<size_t> InitialNumRootBits = None,
         Optional<size_t> InitialNumSubtrieBits = None);

  OnDiskHashMappedTrie(OnDiskHashMappedTrie &&RHS);
  OnDiskHashMappedTrie &operator=(OnDiskHashMappedTrie &&RHS);

  // No copy. Just call \a create() again.
  OnDiskHashMappedTrie(const OnDiskHashMappedTrie &) = delete;
  OnDiskHashMappedTrie &operator=(const OnDiskHashMappedTrie &) = delete;

  ~OnDiskHashMappedTrie();

private:
  struct ImplType;
  explicit OnDiskHashMappedTrie(std::unique_ptr<ImplType> Impl);
  std::unique_ptr<ImplType> Impl;
};

/// On-disk database.
///
/// Top-level layout:
/// - 8-bytes: Magic
/// - 8-bytes: Version
/// - 8-bytes: RootTable (16-bits: Kind; 48-bits: Offset)
/// - 8-bytes: BumpPtr
///
/// Generic table header:
/// - 2-bytes: TableKind
/// - 2-bytes: TableNameSize
/// - 4-bytes: TableNameRelOffset (relative to header)
/// - 8-bytes: Next
class OnDiskDatabase {
public:
  enum TableKind : uint16_t {
    /// OnDiskHashMappedTrie.
    HashMappedTrie = 1,
    /// OnDiskDataStore.
    DataStore = 2,
  };

  FileOffset getRootOffset();
  TableKind getTableKind(FileOffset Offset);
  TableKind getTableKind(FileOffset Offset);

  Expected<OnDiskHashMappedTrie> createRootTrie();

private:
  explicit OnDiskDatabase(std::unique_ptr<ImplType> Impl);
  std::unique_ptr<ImplType> Impl;
};


} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_ONDISKHASHMAPPEDTRIE_H

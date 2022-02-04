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
class OnDiskHashMappedTrie {
public:
  LLVM_DUMP_METHOD void dump() const;
  void print(raw_ostream &OS) const;

public:
  struct ConstValueProxy {
    ConstValueProxy(ArrayRef<uint8_t> Hash, ArrayRef<char> Data)
        : Hash(Hash), Data(Data) {}
    ConstValueProxy(ArrayRef<uint8_t> Hash, StringRef Data)
        : Hash(Hash), Data(Data.begin(), Data.size()) {}

    ArrayRef<uint8_t> Hash;
    ArrayRef<char> Data;
  };

  struct ValueProxy {
    operator ConstValueProxy() const { return ConstValueProxy(Hash, Data); }

    ValueProxy(ArrayRef<uint8_t> Hash, MutableArrayRef<char> Data)
        : Hash(Hash), Data(Data) {}

    ArrayRef<uint8_t> Hash;
    MutableArrayRef<char> Data;
  };

  template <class ProxyT> class PointerImpl {
  public:
    explicit operator bool() const { return Value; }
    const ProxyT &operator*() const { return *Value; }
    const ProxyT *operator->() const { return &*Value; }

    PointerImpl() = default;

  protected:
    PointerImpl(ProxyT Value) : Value(Value), I(-2U) {}
    PointerImpl(void *S, unsigned I, unsigned B) : S(S), I(I), B(B) {}

    bool isHint() const { return I != -1u && I != -2u; }

    Optional<ProxyT> Value;
    const void *S = nullptr;
    unsigned I = -1u;
    unsigned B = 0;
  };

  class pointer;
  class const_pointer : public PointerImpl<ValueProxy> {
  public:
    const_pointer() = default;

  private:
    friend class pointer;
    friend class OnDiskHashMappedTrie;
    using const_pointer::PointerImpl::PointerImpl;
  };

  class pointer : public PointerImpl<ConstValueProxy> {
  public:
    operator const_pointer() const { return const_pointer(Value); }
    pointer() = default;

  private:
    friend class OnDiskHashMappedTrie;
    using pointer::PointerImpl::PointerImpl;
  };

  pointer lookup(ArrayRef<uint8_t> Hash);
  const_pointer lookup(ArrayRef<uint8_t> Hash) const;

  pointer insertLazy(const_pointer Hint, ArrayRef<uint8_t> Hash,
                     function_ref<void(const ValueProxy &)> OnConstruct);
  pointer insertLazy(ArrayRef<uint8_t> Hash,
                     function_ref<void(const ValueProxy &)> OnConstruct) {
    return insertLazy(const_pointer(), Hash, OnConstruct);
  }

  pointer insert(const_pointer Hint, const ConstValueProxy &Value) {
    return insertLazy(Hint, Value.Hash, [&](const ValueProxy &Allocated) {
      assert(Allocated.Hash == Value.Hash);
      assert(Allocated.Data.size() == Value.Data.size());
      llvm::copy(Value.Data, Allocated.Data.begin());
    });
  }
  pointer insert(const ConstValueProxy &Value) {
    return insert(const_pointer(), Value);
  }

  FileOffset getFileOffset(const_pointer P) const;

  /// Gets or creates a file at \p Path with a hash-mapped trie named \p
  /// TrieName. The hash size is \p NumHashBits (in bits) and the records store
  /// data of size \p DataSize (in bytes).
  ///
  /// \p MaxFileSize controls the maximum file size to support, limiting the
  /// size of the \a mapped_file_region. \p NewFileInitialSize is the starting
  /// size if a new file is created.
  ///
  /// \p NewTableNumRootBits and \p NewTableNumSubtrieBits are hints to
  /// configure the trie, if it doesn't already exist.
  ///
  /// \pre NumHashBits is a multiple of 8 (byte-aligned).
  ///
  /// TODO: Expose the internal DatabaseFile abstraction and add support for
  /// adding more tables to a single file.
  ///
  /// FIXME: Rename to getOrCreate().
  static Expected<OnDiskHashMappedTrie>
  create(const Twine &Path, const Twine &TrieName, size_t NumHashBits,
         uint64_t DataSize, uint64_t MaxFileSize,
         Optional<uint64_t> NewFileInitialSize,
         Optional<size_t> NewTableNumRootBits = None,
         Optional<size_t> NewTableNumSubtrieBits = None);

  OnDiskHashMappedTrie(OnDiskHashMappedTrie &&RHS);
  OnDiskHashMappedTrie &operator=(OnDiskHashMappedTrie &&RHS);
  ~OnDiskHashMappedTrie();

private:
  struct ImplType;
  explicit OnDiskHashMappedTrie(std::unique_ptr<ImplType> Impl);
  std::unique_ptr<ImplType> Impl;
};

/// Sink for data. Stores variable length data with 8-byte alignment. Does not
/// track size of data, which is assumed to known from context, or embedded.
/// Uses 0-padding but does not guarantee 0-termination.
class OnDiskDataSink {
public:
  struct ValueProxy {
    MutableArrayRef<char> Data;
    FileOffset Offset;
  };

  /// An iterator-like return value for data insertion. Maybe it should be
  /// called \c iterator, but it has no increment.
  class pointer {
  public:
    explicit operator bool() const { return Value.Offset; }
    const ProxyT &operator*() const {
      assert(Value.Offset && "Null dereference");
      return Value;
    }
    const ProxyT *operator->() const {
      assert(Value.Offset && "Null dereference");
      return &Value;
    }

    pointer() = default;

  private:
    friend class OnDiskDataSink;
    pointer(ValueProxy) : Value(Value) {}
    ValueProxy Value;
  };

  // Look up the data stored at the given offset.
  char *beginData(FileOffset Offset);
  const char *beginData(FileOffset Offset) const;

  pointer allocate(size_t Size);
  pointer save(ArrayRef<char> Data) {
    pointer P = allocate(Data.size());
    llvm::copy(Data, P->begin());
    return P;
  }
  pointer save(StringRef Data) {
    return save(ArrayRef<char>(Data.begin(), Data.size()));
  }

  static Expected<OnDiskDataSink> create(const Twine &Path, StringRef Name,
                                          uint64_t MaxMapSize);

  OnDiskDataSink(OnDiskDataSink &&RHS);
  OnDiskDataSink &operator=(OnDiskDataSink &&RHS);

  // No copy. Just call \a create() again.
  OnDiskDataSink(const OnDiskDataSink &) = delete;
  OnDiskDataSink &operator=(const OnDiskDataSink &) = delete;

  ~OnDiskDataSink();

private:
  struct ImplType;
  explicit OnDiskDataSink(std::unique_ptr<ImplType> Impl);
  std::unique_ptr<ImplType> Impl;
};

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_ONDISKHASHMAPPEDTRIE_H

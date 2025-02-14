//===- HashingOutputBackends.h - Hashing output backends --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_HASHINGOUTPUTBACKEND_H
#define LLVM_SUPPORT_HASHINGOUTPUTBACKEND_H

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/HashBuilder.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputConfig.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace vfs {

/// raw_pwrite_stream that writes to a hasher.
template <typename HasherT>
class HashingStream : public llvm::raw_pwrite_stream {
private:
  SmallVector<char> Buffer;
  raw_svector_ostream OS;

  using HashBuilderT = HashBuilder<HasherT, endianness::native>;
  HashBuilderT Builder;

  void write_impl(const char *Ptr, size_t Size) override {
    OS.write(Ptr, Size);
  }

  void pwrite_impl(const char *Ptr, size_t Size, uint64_t Offset) override {
    OS.pwrite(Ptr, Size, Offset);
  }

  uint64_t current_pos() const override { return OS.str().size(); }

public:
  HashingStream() : OS(Buffer) { SetUnbuffered(); }

  auto final() {
    Builder.update(OS.str());
    return Builder.final();
  }
};

template <typename HasherT> class HashingOutputFile;

/// An output backend that only generates the hash for outputs.
template <typename HasherT> class HashingOutputBackend : public OutputBackend {
private:
  friend class HashingOutputFile<HasherT>;
  void addOutputFile(StringRef Path, StringRef Hash) {
    OutputHashes[Path] = std::string(Hash);
  }

protected:
  IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override {
    return const_cast<HashingOutputBackend<HasherT> *>(this);
  }

  Expected<std::unique_ptr<OutputFileImpl>>
  createFileImpl(StringRef Path, std::optional<OutputConfig> Config) override {
    return std::make_unique<HashingOutputFile<HasherT>>(Path, *this);
  }

public:
  /// Iterator for all the output file names.
  auto outputFiles() const { return OutputHashes.keys(); }

  /// Get hash value for the output files in hex representation.
  /// Return None if the requested path is not generated.
  std::optional<std::string> getHashValueForFile(StringRef Path) {
    auto F = OutputHashes.find(Path);
    if (F == OutputHashes.end())
      return std::nullopt;
    return toHex(F->second);
  }

private:
  StringMap<std::string> OutputHashes;
};

/// HashingOutputFile.
template <typename HasherT>
class HashingOutputFile final : public OutputFileImpl {
public:
  Error keep() override {
    auto Result = OS.final();
    Backend.addOutputFile(OutputPath, toStringRef(Result));
    return Error::success();
  }
  Error discard() override { return Error::success(); }
  raw_pwrite_stream &getOS() override { return OS; }

  HashingOutputFile(StringRef OutputPath,
                    HashingOutputBackend<HasherT> &Backend)
      : OutputPath(OutputPath.str()), Backend(Backend) {}

private:
  const std::string OutputPath;
  HashingStream<HasherT> OS;
  HashingOutputBackend<HasherT> &Backend;
};

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_HASHINGOUTPUTBACKEND_H

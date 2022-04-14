//===- CASOutputBackend.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASOutputBackend.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/Utils.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Allocator.h"

using namespace llvm;
using namespace llvm::cas;

namespace {
class CASOutputFile : public vfs::OutputFile {
public:
  Error storeContentBuffer(ContentBuffer &Content) override {
    return OnClose(getPath(), Content.getBytes());
  }

  using OnCloseType = llvm::unique_function<Error(StringRef, StringRef)>;

  CASOutputFile(StringRef Path, OnCloseType OnClose)
      : OutputFile(Path), OnClose(std::move(OnClose)) {}

  static bool isNull(const OutputFile &File) {
    return OutputFile::isNull(File);
  }

private:
  OnCloseType OnClose;
};
} // namespace

CASOutputBackend::CASOutputBackend(std::shared_ptr<CASDB> CAS)
    : CASOutputBackend(*CAS) {
  this->OwnedCAS = std::move(CAS);
}

CASOutputBackend::CASOutputBackend(CASDB &CAS)
    : StableUniqueEntityAdaptorType(
          sys::path::Style::native /*FIXME: should be posix?*/),
      CAS(CAS) {}

CASOutputBackend::~CASOutputBackend() = default;

struct CASOutputBackend::PrivateImpl {
  // FIXME: Use a NodeBuilder here once it exists.
  SmallVector<CASID> IDs;
};

Expected<std::unique_ptr<vfs::OutputFile>>
CASOutputBackend::createFileImpl(StringRef ResolvedPath,
                                 vfs::OutputConfig Config) {
  if (!Impl)
    Impl = std::make_unique<PrivateImpl>();

  return std::make_unique<CASOutputFile>(
      ResolvedPath, [&](StringRef Path, StringRef Bytes) -> Error {
        Optional<BlobProxy> PathBlob;
        Optional<BlobProxy> BytesBlob;
        if (Error E = CAS.createBlob(Path).moveInto(PathBlob))
          return E;
        if (Error E = CAS.createBlob(Bytes).moveInto(BytesBlob))
          return E;

        // FIXME: Should there be a lock taken before accessing PrivateImpl?
        Impl->IDs.push_back(PathBlob->getID());
        Impl->IDs.push_back(BytesBlob->getID());
        return Error::success();
      });
}

Expected<NodeProxy> CASOutputBackend::createNode() {
  // FIXME: Should there be a lock taken before accessing PrivateImpl?
  if (!Impl)
    return CAS.createNode(None, "");

  SmallVector<CASID> MovedIDs;
  std::swap(MovedIDs, Impl->IDs);
  return CAS.createNode(MovedIDs, "");
}

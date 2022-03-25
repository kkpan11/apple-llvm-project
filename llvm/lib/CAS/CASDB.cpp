//===- CASDB.cpp ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASDB.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"

using namespace llvm;
using namespace llvm::cas;

void CASIDContext::anchor() {}
void CASDB::anchor() {}

LLVM_DUMP_METHOD void CASID::dump() const { print(dbgs()); }
LLVM_DUMP_METHOD void CASDB::dump() const { print(dbgs()); }
LLVM_DUMP_METHOD void ObjectRef::dump() const { print(dbgs()); }
LLVM_DUMP_METHOD void ObjectHandle::dump() const { print(dbgs()); }

std::string CASID::toString() const {
  std::string S;
  raw_string_ostream(S) << *this;
  return S;
}

static void printReferenceBase(raw_ostream &OS, StringRef Kind,
                               uint64_t InternalRef, Optional<CASID> ID) {
  OS << Kind << "=" << InternalRef;
  if (ID)
    OS << "[" << *ID << "]";
}

void ReferenceBase::print(raw_ostream &OS, const ObjectHandle &This) const {
  assert(this == &This);

  Optional<CASID> ID;
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  if (CAS)
    ID = CAS->getObjectID(This);
#endif
  printReferenceBase(OS, "object-handle", InternalRef, ID);
}

void ReferenceBase::print(raw_ostream &OS, const ObjectRef &This) const {
  assert(this == &This);

  Optional<CASID> ID;
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  if (CAS)
    ID = CAS->getObjectID(This);
#endif
  printReferenceBase(OS, "object-ref", InternalRef, ID);
}

Expected<BlobProxy>
CASDB::createBlobFromOpenFile(sys::fs::file_t FD,
                              Optional<sys::fs::file_status> Status) {
  if (Expected<BlobHandle> Blob = storeBlobFromOpenFile(FD, Status))
    return BlobProxy::load(*this, *Blob);
  else
    return Blob.takeError();
}

/// Default implementation opens the file and calls \a createBlob().
Expected<BlobHandle>
CASDB::storeBlobFromOpenFileImpl(sys::fs::file_t FD,
                                 Optional<sys::fs::file_status> Status) {
  // Check whether we can trust the size from stat.
  int64_t FileSize = -1;
  if (Status->type() == sys::fs::file_type::regular_file ||
      Status->type() == sys::fs::file_type::block_file)
    FileSize = Status->getSize();

  // No need for a null terminator since the buffer will be dropped.
  ErrorOr<std::unique_ptr<MemoryBuffer>> ExpectedContent =
      MemoryBuffer::getOpenFile(FD, /*Filename=*/"", FileSize,
                                /*RequiresNullTerminator=*/false);
  if (!ExpectedContent)
    return errorCodeToError(ExpectedContent.getError());

  return storeBlob((*ExpectedContent)->getBuffer());
}

Expected<Optional<AnyObjectHandle>> CASDB::loadObject(const CASID &ID) {
  if (Optional<ObjectRef> Ref = getReference(ID))
    return loadObject(*Ref);
  return None;
}

void CASDB::readRefs(NodeHandle Node, SmallVectorImpl<ObjectRef> &Refs) const {
  consumeError(forEachRef(Node, [&Refs](ObjectRef Ref) -> Error {
    Refs.push_back(Ref);
    return Error::success();
  }));
}

template <class ProxyT, class HandleT>
Expected<ProxyT> CASDB::loadObjectProxy(CASID ID) {
  Optional<AnyObjectHandle> H;
  if (Error E = loadObject(ID).moveInto(H))
    return std::move(E);
  if (!H)
    return createUnknownObjectError(ID);
  if (Optional<HandleT> Casted = H->dyn_cast<HandleT>())
    return ProxyT::load(*this, *Casted);
  return createWrongKindError(ID);
}

template <class ProxyT, class HandleT>
Expected<ProxyT> CASDB::loadObjectProxy(Expected<HandleT> H) {
  if (!H)
    return H.takeError();
  return ProxyT::load(*this, *H);
}

Expected<BlobProxy> CASDB::getBlob(CASID ID) {
  return loadObjectProxy<BlobProxy, BlobHandle>(ID);
}
Expected<TreeProxy> CASDB::getTree(CASID ID) {
  return loadObjectProxy<TreeProxy, TreeHandle>(ID);
}
Expected<NodeProxy> CASDB::getNode(CASID ID) {
  return loadObjectProxy<NodeProxy, NodeHandle>(ID);
}

Error CASDB::createUnknownObjectError(CASID ID) {
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "unknown object '" + ID.toString() + "'");
}

Error CASDB::createWrongKindError(CASID ID) {
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           "wrong object kind '" + ID.toString() + "'");
}

Expected<BlobProxy> CASDB::createBlob(StringRef Data) {
  return loadObjectProxy<BlobProxy>(storeBlob(Data));
}

Expected<TreeProxy> CASDB::createTree(ArrayRef<NamedTreeEntry> Entries) {
  return loadObjectProxy<TreeProxy>(storeTree(Entries));
}

Expected<NodeProxy> CASDB::createNode(ArrayRef<CASID> IDs, StringRef Data) {
  SmallVector<ObjectRef> Refs;
  for (CASID ID : IDs) {
    if (Optional<ObjectRef> Ref = getReference(ID))
      Refs.push_back(*Ref);
    else
      return createUnknownObjectError(ID);
  }
  return loadObjectProxy<NodeProxy>(
      storeNode(Refs, arrayRefFromStringRef<char>(Data)));
}

Expected<std::unique_ptr<MemoryBuffer>>
CASDB::loadIndependentDataBuffer(AnyDataHandle Data, const Twine &Name,
                                 bool NullTerminate) const {
  return loadIndependentDataBufferImpl(Data, Name, NullTerminate);
}

Expected<std::unique_ptr<MemoryBuffer>>
CASDB::loadIndependentDataBufferImpl(AnyDataHandle Data, const Twine &Name,
                                     bool NullTerminate) const {
  SmallString<256> Bytes;
  raw_svector_ostream OS(Bytes);
  readData(Data, OS);
  return std::make_unique<SmallVectorMemoryBuffer>(std::move(Bytes), Name.str(),
                                                   NullTerminate);
}

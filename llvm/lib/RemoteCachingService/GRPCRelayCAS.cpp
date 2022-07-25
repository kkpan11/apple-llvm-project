//===- GRPCRelayCAS.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/LazyAtomicPointer.h"
#include "llvm/CAS/CASID.h"
#include "llvm/CAS/HashMappedTrie.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/CAS/ThreadSafeAllocator.h"
#include "llvm/Config/config.h"
#include "llvm/RemoteCachingService/RemoteCachingService.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

#define DEBUG_TYPE "grpc-cas"

#if LLVM_ENABLE_GRPC_CAS

#include "compilation_caching_cas.grpc.pb.h"
#include "compilation_caching_kv.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>

using namespace llvm;
using namespace llvm::cas;
using namespace compilation_cache_service::cas::v1;
using namespace compilation_cache_service::keyvalue::v1;
using namespace grpc;

namespace {

class InMemoryCASData;
// The in memory HashMappedTrie to store CASData from Service.
// This implementation assumes 80 byte hash max.
using InMemoryIndexT =
    ThreadSafeHashMappedTrie<LazyAtomicPointer<const InMemoryCASData>, 80>;
using InMemoryIndexValueT = InMemoryIndexT::value_type;

// InMemoryCASData.
// Store CASData together with the hash in the index.
class InMemoryCASData {
public:
  ArrayRef<uint8_t> getHash() const { return Index.Hash; }

  InMemoryCASData() = delete;
  InMemoryCASData(InMemoryCASData &&) = delete;
  InMemoryCASData(const InMemoryCASData &) = delete;

  ArrayRef<const InMemoryIndexValueT *> getRefs() const {
    return makeArrayRef(
        reinterpret_cast<const InMemoryIndexValueT *const *>(this + 1),
        NumRefs);
  }

  ArrayRef<char> getData() const {
    ArrayRef<const InMemoryIndexValueT *> Refs = getRefs();
    return makeArrayRef(
        reinterpret_cast<const char *>(Refs.data() + Refs.size()), DataSize);
  }

  uint64_t getDataSize() const { return DataSize; }
  size_t getNumRefs() const { return NumRefs; }

  const InMemoryIndexValueT &getIndex() const { return Index; }

  static InMemoryCASData &create(function_ref<void *(size_t Size)> Allocate,
                                 const InMemoryIndexValueT &I,
                                 ArrayRef<const InMemoryIndexValueT *> Refs,
                                 ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryCASData) +
                         sizeof(uintptr_t) * Refs.size() + Data.size() + 1);
    return *new (Mem) InMemoryCASData(I, Refs, Data);
  }

private:
  InMemoryCASData(const InMemoryIndexValueT &I,
                  ArrayRef<const InMemoryIndexValueT *> Refs,
                  ArrayRef<char> Data)
      : Index(I), NumRefs(Refs.size()), DataSize(Data.size()) {
    auto *BeginRefs = reinterpret_cast<const InMemoryIndexValueT **>(this + 1);
    llvm::copy(Refs, BeginRefs);
    auto *BeginData = reinterpret_cast<char *>(BeginRefs + NumRefs);
    llvm::copy(Data, BeginData);
    BeginData[Data.size()] = 0;
  }

  const InMemoryIndexValueT &Index;
  uint32_t NumRefs;
  uint32_t DataSize;
};

class GRPCCASContext : public CASContext {
  void printIDImpl(raw_ostream &OS, const CASID &ID) const final;
  void anchor() override;

public:
  static StringRef getHashName() { return "Luxon"; }
  StringRef getHashSchemaIdentifier() const final {
    static const std::string ID =
        ("grpc::cas::service::v1[" + getHashName() + "]").str();
    return ID;
  }

  static const GRPCCASContext &getDefaultContext();

  GRPCCASContext() = default;
};

class GRPCRelayCAS : public ObjectStore {
public:
  GRPCRelayCAS(StringRef Path, Error &Err);

  ArrayRef<uint8_t> getHashImpl(const CASID &ID) const;

  // ObjectStore interfaces.
  Expected<CASID> parseID(StringRef ID) final;
  Expected<ObjectRef> store(ArrayRef<ObjectRef> Refs,
                               ArrayRef<char> Data) final;
  CASID getID(ObjectRef Ref) const final;
  CASID getID(ObjectHandle Handle) const final;
  Optional<ObjectRef> getReference(const CASID &ID) const final;
  ObjectRef getReference(ObjectHandle Handle) const final;
  Expected<ObjectHandle> load(ObjectRef Ref) final;
  Error validate(const CASID &ID) final {
    // Not supported yet. Always return success.
    return Error::success();
  }
  uint64_t getDataSize(ObjectHandle Node) const final;
  Error forEachRef(ObjectHandle Node,
                   function_ref<Error(ObjectRef)> Callback) const final;
  ObjectRef readRef(ObjectHandle Node, size_t I) const final;
  size_t getNumRefs(ObjectHandle Node) const final;
  ArrayRef<char> getData(ObjectHandle Node,
                         bool RequiresNullTerminator = false) const final;

  // For sending file path through grpc.
  Expected<ObjectRef>
  storeFromOpenFileImpl(sys::fs::file_t FD,
                        Optional<sys::fs::file_status> Status) override;

  // API that uses the file API.
  Expected<ObjectRef> storeFile(StringRef Content);
  Expected<StringRef> loadFile(ObjectRef Node);

private:
  InMemoryIndexValueT &indexHash(ArrayRef<uint8_t> Hash) const {
    assert(Hash.size() == ServiceHashSize && "unexpected hash size");
    SmallVector<uint8_t, sizeof(InMemoryIndexT::HashT)> ExtendedHash(
        sizeof(InMemoryIndexT::HashT));
    llvm::copy(Hash, ExtendedHash.begin());
    return *Index.insertLazy(ExtendedHash, [](auto ValueConstructor) {
      ValueConstructor.emplace(nullptr);
    });
  }

  CASID getID(const InMemoryIndexValueT &I) const {
    ArrayRef<uint8_t> Hash = I.Data->getHash().take_front(ServiceHashSize);
    return CASID::create(&getContext(), toStringRef(Hash));
  }

  const InMemoryCASData &asInMemoryCASData(ObjectHandle Ref) const {
    uintptr_t P = Ref.getInternalRef(*this);
    return *reinterpret_cast<const InMemoryCASData *>(P);
  }

  InMemoryIndexValueT &asInMemoryIndexValue(ObjectRef Ref) const {
    uintptr_t P = Ref.getInternalRef(*this);
    return *reinterpret_cast<InMemoryIndexValueT *>(P);
  }

  ObjectRef toReference(const InMemoryCASData &O) const {
    return toReference(O.getIndex());
  }

  ObjectRef toReference(const InMemoryIndexValueT &I) const {
    return makeObjectRef(reinterpret_cast<uintptr_t>(&I));
  }

  const InMemoryCASData &getInMemoryCASData(ObjectHandle OH) const {
    return *reinterpret_cast<const InMemoryCASData *>(
        (uintptr_t)OH.getInternalRef(*this));
  }

  ObjectHandle getObjectHandle(const InMemoryCASData &Node) const {
    assert(!(reinterpret_cast<uintptr_t>(&Node) & 0x1ULL));
    return makeObjectHandle(reinterpret_cast<uintptr_t>(&Node));
  }

  const InMemoryCASData &
  storeObjectImpl(InMemoryIndexValueT &I,
                  ArrayRef<const InMemoryIndexValueT *> Refs,
                  ArrayRef<char> Data) {
    // Load or generate.
    auto Allocator = [&](size_t Size) -> void * {
      return Alloc.Allocate(Size, alignof(InMemoryCASData));
    };
    auto Generator = [&]() -> const InMemoryCASData * {
      return &InMemoryCASData::create(Allocator, I, Refs, Data);
    };
    return I.Data.loadOrGenerate(Generator);
  }

  CASDataID createCASDataID(ObjectRef Ref) {
    StringRef Hash =
        toStringRef(asInMemoryIndexValue(Ref).Hash).take_front(ServiceHashSize);
    CASDataID ID;
    ID.set_id(Hash.str());
    return ID;
  }

  CASObject createCASObject(ArrayRef<ObjectRef> Refs, ArrayRef<char> Data) {
    CASObject D;
    D.mutable_blob()->set_data(toStringRef(Data).str());
    for (ObjectRef Ref : Refs) {
      CASDataID *NewRef = D.add_references();
      StringRef HashStr = toStringRef(asInMemoryIndexValue(Ref).Hash)
                              .take_front(ServiceHashSize);
      NewRef->set_id(HashStr.str());
    }
    return D;
  }

  CASBlob createCASBlob(StringRef Data) {
    CASBlob D;
    D.mutable_blob()->set_data(Data.str());
    return D;
  }

  CASObject createCASObject(StringRef Path) {
    CASObject D;
    D.mutable_blob()->set_file_path(Path.str());
    return D;
  }

  Error createUnknownObjectError(ObjectRef Ref) const {
    CASID ID = getID(Ref);
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "unknown object '" + ID.toString() + "'");
  }

  Error createGRPCResponseError(
      const compilation_cache_service::cas::v1::ResponseError &RE) const {
    return createStringError(std::make_error_code(std::errc::bad_message),
                             "grpc error: " + RE.description());
  }

  // Stub for compile_cache_service.
  std::unique_ptr<CASDBService::Stub> CASStub;

  // Index to manage the remote CAS.
  mutable InMemoryIndexT Index;
  // Allocator for CAS content.
  ThreadSafeAllocator<BumpPtrAllocator> Alloc;

  // Store the size of the hash.
  size_t ServiceHashSize;
};

class GRPCActionCache : public ActionCache {
public:
  GRPCActionCache(StringRef Path);

  Expected<Optional<CASID>> getImpl(ArrayRef<uint8_t> ResolvedKey) const final;
  Error putImpl(ArrayRef<uint8_t> ResolvedKey, const CASID &Result) final;

private:
  Error createGRPCResponseError(
      const compilation_cache_service::keyvalue::v1::ResponseError &RE) const {
    return createStringError(std::make_error_code(std::errc::bad_message),
                             "grpc error: " + RE.description());
  }

  std::unique_ptr<KeyValueDB::Stub> KVStub;
};

} // namespace

static Error createGRPCStatusError(const Status &Status) {
  if (Status.ok())
    return Error::success();
  return createStringError(std::make_error_code(std::errc::connection_aborted),
                           "grpc error (" + toStringRef(Status.error_code()) +
                               "): " + Status.error_message());
}

const GRPCCASContext &GRPCCASContext::getDefaultContext() {
  static GRPCCASContext DefaultContext;
  return DefaultContext;
}
void GRPCCASContext::anchor() {}

GRPCRelayCAS::GRPCRelayCAS(StringRef Path, Error &Err)
    : ObjectStore(GRPCCASContext::getDefaultContext()) {
  ErrorAsOutParameter ErrAsOutParam(&Err);
  std::string Address("unix:");
  Address += Path;
  ChannelArguments Args;
  Args.SetMaxReceiveMessageSize(INT_MAX);
  Args.SetMaxSendMessageSize(INT_MAX);
  const auto Channel = grpc::CreateCustomChannel(
      std::move(Address), grpc::InsecureChannelCredentials(), Args);
  CASStub = CASDBService::NewStub(Channel);

  // Send a put request to the CAS to determine hash size.
  CASPutRequest Request;
  ClientContext Context;
  CASPutResponse Response;
  Request.mutable_data()->CopyFrom(createCASObject(None, ""));
  grpc::Status Status = CASStub->Put(&Context, Request, &Response);
  if (!Status.ok()) {
    Err = createGRPCStatusError(Status);
    return;
  }
  if (Response.has_error()) {
    Err = createGRPCResponseError(Response.error());
    return;
  }
  ServiceHashSize = Response.cas_id().id().size();
  if (ServiceHashSize > 80)
    Err = createStringError(std::make_error_code(std::errc::message_size),
                            "Hash from CASService is too long");
}

ArrayRef<uint8_t> GRPCRelayCAS::getHashImpl(const CASID &ID) const {
  ArrayRef<uint8_t> ExtendedHash(ID.getHash());
  return ExtendedHash.take_front(ServiceHashSize);
}

// FIXME: Just print out as hexdump for now. Should move this into GRPC
// protocol.
void GRPCCASContext::printIDImpl(raw_ostream &OS, const CASID &ID) const {
  assert(&ID.getContext() == this);
  SmallString<64> Hash;
  toHex(ID.getHash(), /*LowerCase=*/true, Hash);
  OS << Hash;
}

Expected<CASID> GRPCRelayCAS::parseID(StringRef Reference) {
  std::string Binary;
  if (!tryGetFromHex(Reference, Binary))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid hash in cas-id '" + Reference + "'");
  return getID(indexHash(arrayRefFromStringRef(Binary)));
}

Expected<ObjectRef> GRPCRelayCAS::store(ArrayRef<ObjectRef> Refs,
                                        ArrayRef<char> Data) {
  // Send CASPutRequest.
  // TODO: Async API. Maybe check local CAS first to decide if needs to go
  // remote?
  CASPutRequest Request;
  ClientContext Context;
  CASPutResponse Response;
  Request.mutable_data()->CopyFrom(createCASObject(Refs, Data));
  grpc::Status Status = CASStub->Put(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);

  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  auto &I = indexHash(arrayRefFromStringRef(Response.cas_id().id()));
  // Create the node.
  SmallVector<const InMemoryIndexValueT *> InternalRefs;
  for (ObjectRef Ref : Refs)
    InternalRefs.push_back(&asInMemoryIndexValue(Ref));

  return toReference(storeObjectImpl(I, InternalRefs, Data));
}

CASID GRPCRelayCAS::getID(ObjectRef Ref) const {
  return getID(asInMemoryIndexValue(Ref));
}

CASID GRPCRelayCAS::getID(ObjectHandle Handle) const {
  return getID(asInMemoryCASData(Handle).getIndex());
}

Optional<ObjectRef> GRPCRelayCAS::getReference(const CASID &ID) const {
  assert(ID.getContext().getHashSchemaIdentifier() ==
             getContext().getHashSchemaIdentifier() &&
         "Expected ID from same hash schema");
  auto &I = indexHash(ID.getHash());
  return toReference(I);
}

ObjectRef GRPCRelayCAS::getReference(ObjectHandle Handle) const {
  return toReference(asInMemoryCASData(Handle));
}

Expected<ObjectHandle> GRPCRelayCAS::load(ObjectRef Ref) {
  auto &I = asInMemoryIndexValue(Ref);

  // Return the existing value if we have one.
  if (const InMemoryCASData *Existing = I.Data.load())
    return getObjectHandle(*Existing);

  // Send the request to remote to load the object. Since Generator function
  // can't return nullptr to indicate failed load now, we can't lock the entry
  // into busy state. we will send parallel loads and put one of the result into
  // the local CAS.
  CASGetRequest Request;
  ClientContext Context;
  CASGetResponse Response;
  Request.mutable_cas_id()->CopyFrom(createCASDataID(Ref));
  Request.set_write_to_disk(false);
  grpc::Status Status = CASStub->Get(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);
  auto Outcome = Response.outcome();
  if (Outcome == CASGetResponse_Outcome_OBJECT_NOT_FOUND)
    return createUnknownObjectError(Ref);
  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  SmallVector<const InMemoryIndexValueT *> InternalRefs;
  for (const CASDataID &ID : Response.data().references()) {
    auto &Ref = indexHash(arrayRefFromStringRef(ID.id()));
    InternalRefs.push_back(&Ref);
  }
  if (Response.data().blob().has_file_path()) {
    auto MB = MemoryBuffer::getFile(Response.data().blob().file_path());
    if (!MB)
      return errorCodeToError(MB.getError());
    return getObjectHandle(storeObjectImpl(
        I, InternalRefs, arrayRefFromStringRef<char>((*MB)->getBuffer())));
  }
  std::string Data = Response.data().blob().data();
  ArrayRef<char> Content(Data.data(), Data.size());
  return getObjectHandle(storeObjectImpl(I, InternalRefs, Content));
}

uint64_t GRPCRelayCAS::getDataSize(ObjectHandle Handle) const {
  return getInMemoryCASData(Handle).getDataSize();
}

Error GRPCRelayCAS::forEachRef(ObjectHandle Handle,
                               function_ref<Error(ObjectRef)> Callback) const {
  auto &Node = getInMemoryCASData(Handle);
  for (const InMemoryIndexValueT *Ref : Node.getRefs())
    if (Error E = Callback(toReference(*Ref)))
      return E;
  return Error::success();
}

ObjectRef GRPCRelayCAS::readRef(ObjectHandle Node, size_t I) const {
  return toReference(*getInMemoryCASData(Node).getRefs()[I]);
}

size_t GRPCRelayCAS::getNumRefs(ObjectHandle Handle) const {
  return getInMemoryCASData(Handle).getNumRefs();
}

ArrayRef<char> GRPCRelayCAS::getData(ObjectHandle Handle,
                                     bool RequiresNullTerminator) const {
  return getInMemoryCASData(Handle).getData();
}

Expected<ObjectRef>
GRPCRelayCAS::storeFromOpenFileImpl(sys::fs::file_t FD,
                                    Optional<sys::fs::file_status> FS) {
  std::error_code EC;
  sys::fs::mapped_file_region Map(FD, sys::fs::mapped_file_region::readonly,
                                  FS->getSize(),
                                  /*offset=*/0, EC);
  if (EC)
    return errorCodeToError(EC);

  ArrayRef<char> Data(Map.data(), Map.size());
  CASPutRequest Request;
  ClientContext Context;
  CASPutResponse Response;
  SmallString<128> Path;
  if (sys::fs::getRealPathFromHandle(FD, Path))
    Request.mutable_data()->CopyFrom(createCASObject(Path));
  else
    Request.mutable_data()->CopyFrom(createCASObject(None, Data));


  grpc::Status Status = CASStub->Put(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);
  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  auto &I = indexHash(arrayRefFromStringRef(Response.cas_id().id()));
  // TODO: we can avoid the copy by implementing InMemoryRef object like
  // InMemoryCAS.
  return toReference(storeObjectImpl(I, None, Data));
}

Expected<ObjectRef> GRPCRelayCAS::storeFile(StringRef Content) {
  CASSaveRequest Request;
  ClientContext Context;
  CASSaveResponse Response;
  Request.mutable_data()->CopyFrom(createCASBlob(Content));
  grpc::Status Status = CASStub->Save(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);

  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  auto &I = indexHash(arrayRefFromStringRef(Response.cas_id().id()));
  ArrayRef<char> Data = arrayRefFromStringRef<char>(Content);
  return toReference(storeObjectImpl(I, {}, Data));
}

Expected<StringRef> GRPCRelayCAS::loadFile(ObjectRef Ref) {
  auto &I = asInMemoryIndexValue(Ref);

  // Return the existing value if we have one.
  if (const InMemoryCASData *Existing = I.Data.load()) {
    assert(Existing->getNumRefs() == 0 && "File should have zero refs");
    return toStringRef(Existing->getData());
  }

  CASLoadRequest Request;
  ClientContext Context;
  CASLoadResponse Response;
  Request.mutable_cas_id()->CopyFrom(createCASDataID(Ref));
  Request.set_write_to_disk(false);
  grpc::Status Status = CASStub->Load(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);
  auto Outcome = Response.outcome();
  if (Outcome == CASLoadResponse_Outcome_OBJECT_NOT_FOUND)
    return createUnknownObjectError(Ref);
  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  auto getDataAsStringRef =
      [&](Expected<ObjectHandle> Handle) -> Expected<StringRef> {
    if (!Handle)
      return Handle.takeError();
    return toStringRef(getData(*Handle));
  };
  if (Response.data().blob().has_file_path()) {
    auto MB = MemoryBuffer::getFile(Response.data().blob().file_path());
    if (!MB)
      return errorCodeToError(MB.getError());
    return getDataAsStringRef(getObjectHandle(storeObjectImpl(
        I, None, arrayRefFromStringRef<char>((*MB)->getBuffer()))));
  }
  std::string Data = Response.data().blob().data();
  ArrayRef<char> Content(Data.data(), Data.size());
  return getDataAsStringRef(getObjectHandle(storeObjectImpl(I, {}, Content)));
}

GRPCActionCache::GRPCActionCache(StringRef Path)
    : ActionCache(GRPCCASContext::getDefaultContext()) {
  std::string Address("unix:");
  Address += Path;
  ChannelArguments Args;
  const auto Channel = grpc::CreateCustomChannel(
      std::move(Address), grpc::InsecureChannelCredentials(), Args);
  KVStub = KeyValueDB::NewStub(std::move(Channel));
}

Expected<Optional<CASID>>
GRPCActionCache::getImpl(ArrayRef<uint8_t> ResolvedKey) const {
  GetValueRequest Request;
  ClientContext Context;
  GetValueResponse Response;
  Request.set_key(toStringRef(ResolvedKey).str());
  grpc::Status Status = KVStub->GetValue(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);

  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  auto Outcome = Response.outcome();
  if (Outcome == GetValueResponse_Outcome_KEY_NOT_FOUND)
    return None;

  std::string Value = Response.value().entries().at("CASID");
  return CASID::create(&getContext(), Value);
}

Error GRPCActionCache::putImpl(ArrayRef<uint8_t> ResolvedKey,
                               const CASID &Result) {
  PutValueRequest Request;
  ClientContext Context;
  PutValueResponse Response;
  std::string InputStr = toStringRef(ResolvedKey).str();
  std::string OutputStr = toStringRef(Result.getHash()).str();
  // Store the CASID in the CASID dictionary field.
  Request.mutable_value()->mutable_entries()->insert({"CASID", OutputStr});
  Request.set_key(InputStr);
  grpc::Status Status = KVStub->PutValue(&Context, Request, &Response);
  if (!Status.ok())
    return createGRPCStatusError(Status);

  if (Response.has_error())
    return createGRPCResponseError(Response.error());

  return Error::success();
}

Expected<std::unique_ptr<ObjectStore>>
cas::createGRPCRelayCAS(const Twine &Path) {
  Error Err = Error::success();
  auto CAS = std::make_unique<GRPCRelayCAS>(Path.str(), Err);
  if (Err)
    return std::move(Err);

  return CAS;
}

Expected<std::unique_ptr<cas::ActionCache>>
cas::createGRPCActionCache(StringRef Path) {
  return std::make_unique<GRPCActionCache>(Path);
}

RegisterGRPCCAS::RegisterGRPCCAS() {
  cas::registerCASURLScheme("grpc://", &cas::createGRPCRelayCAS);
}

#else /* LLVM_ENABLE_GRPC_CAS */

using namespace llvm;

Expected<std::unique_ptr<cas::ObjectStore>>
cas::createGRPCRelayCAS(const Twine &Path) {
  return createStringError(inconvertibleErrorCode(),
                           "GRPCRelayCAS is disabled");
}

Expected<std::unique_ptr<cas::ActionCache>>
cas::createGRPCActionCache(StringRef Path) {
  return createStringError(inconvertibleErrorCode(),
                           "GRPCActionCache is disabled");
}

cas::RegisterGRPCCAS::RegisterGRPCCAS() {}

#endif /* LLVM_ENABLE_GRPC_CAS */

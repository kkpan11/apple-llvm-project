//===- MockGRPCServer.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/thread.h"
#include <condition_variable>
#include <mutex>

#include "CASTestConfig.h"

#if LLVM_ENABLE_GRPC_CAS

#include "compilation_caching_cas.grpc.pb.h"
#include "compilation_caching_kv.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>

using namespace llvm;
using namespace compilation_cache_service::cas::v1;
using namespace compilation_cache_service::keyvalue::v1;
using namespace grpc;

namespace {

class MockKeyValueImpl final : public KeyValueDB::Service {
public:
  Status GetValue(ServerContext *Context, const GetValueRequest *Request,
                  GetValueResponse *Response) override;
  Status PutValue(ServerContext *Context, const PutValueRequest *Request,
                  PutValueResponse *Response) override;

private:
  StringMap<std::string> MockStorage;
};

class MockCASImpl final : public CASDBService::Service {
public:
  Status Get(ServerContext *Context, const CASGetRequest *Request,
             CASGetResponse *Response) override;
  Status Put(ServerContext *Context, const CASPutRequest *Request,
             CASPutResponse *Response) override;
  Status Load(ServerContext *Context, const CASLoadRequest *Request,
              CASLoadResponse *Response) override;
  Status Save(ServerContext *Context, const CASSaveRequest *Request,
              CASSaveResponse *Response) override;

  MockCASImpl() : MockDB(cas::createInMemoryCAS()) {}

private:
  std::unique_ptr<cas::ObjectStore> MockDB;
};

class MockEnvImpl final : public unittest::cas::MockEnv {
public:
  MockEnvImpl(std::string Path);
  ~MockEnvImpl();

private:
  std::shared_ptr<Server> ServerRef;
  std::unique_ptr<thread> Thread;
};

} // namespace

Status MockKeyValueImpl::GetValue(ServerContext *Context,
                                  const GetValueRequest *Request,
                                  GetValueResponse *Response) {
  std::string Key = Request->key();
  auto Value = MockStorage.find(Key);
  if (Value == MockStorage.end()) {
    Response->set_outcome(GetValueResponse_Outcome_KEY_NOT_FOUND);
    return Status::OK;
  }
  Response->set_outcome(GetValueResponse_Outcome_SUCCESS);
  Response->mutable_value()->mutable_entries()->insert(
      {"CASID", Value->second});
  return Status::OK;
}

Status MockKeyValueImpl::PutValue(ServerContext *Context,
                                  const PutValueRequest *Request,
                                  PutValueResponse *Response) {
  std::string Key = Request->key();
  std::string Value = Request->value().entries().at("CASID");
  auto Result = MockStorage.try_emplace(Key, Value);
  if (!Result.second && Result.first->getValue() != Value)
    // Poisoned value. Return error.
    Response->mutable_error()->set_description("value already existed");
  return Status::OK;
}

static CASDataID getCASDataID(cas::CASID ID) {
  CASDataID Value;
  SmallVector<uint8_t> Hash(ID.getHash().begin(), ID.getHash().end());
  Value.set_id(toStringRef(Hash).str());
  return Value;
}

static Expected<cas::CASID> getCASIDFromDataID(const CASDataID &ID,
                                               cas::ObjectStore &DB) {
  SmallString<64> Hash;
  toHex(arrayRefFromStringRef(ID.id()), /*LowerCase=*/true, Hash);
  std::string IDStr;
  raw_string_ostream SS(IDStr);
  SS << "llvmcas://" << Hash;
  return DB.parseID(IDStr);
}

Status MockCASImpl::Get(ServerContext *Context, const CASGetRequest *Request,
                        CASGetResponse *Response) {
  // Get by converting to a CASID string first, then fetch.
  auto CASID = getCASIDFromDataID(Request->cas_id(), *MockDB);
  if (!CASID) {
    Response->set_outcome(CASGetResponse_Outcome_ERROR);
    Response->mutable_error()->set_description(toString(CASID.takeError()));
    return Status::OK;
  }
  auto Proxy = MockDB->getProxy(*CASID);
  if (!Proxy) {
    Response->set_outcome(CASGetResponse_Outcome_ERROR);
    Response->mutable_error()->set_description(toString(Proxy.takeError()));
    return Status::OK;
  }
  Response->mutable_data()->mutable_blob()->set_data(Proxy->getData().str());
  auto Err = Proxy->forEachReference([&](cas::ObjectRef Ref) -> Error {
    cas::CASID ID = Proxy->getCAS().getID(Ref);
    Response->mutable_data()->mutable_references()->Add(getCASDataID(ID));
    return Error::success();
  });
  if (Err) {
    Response->set_outcome(CASGetResponse_Outcome_ERROR);
    Response->mutable_error()->set_description(toString(std::move(Err)));
  }
  Response->set_outcome(CASGetResponse_Outcome_SUCCESS);
  return Status::OK;
}

Status MockCASImpl::Put(ServerContext *Context, const CASPutRequest *Request,
                        CASPutResponse *Response) {
  ArrayRef<char> Data =
      arrayRefFromStringRef<char>(Request->data().blob().data());
  SmallVector<cas::ObjectRef> Refs;
  for (auto R : Request->data().references()) {
    auto ID = getCASIDFromDataID(R, *MockDB);
    if (!ID) {
      Response->mutable_error()->set_description(toString(ID.takeError()));
      return Status::OK;
    }
    auto Ref = MockDB->getReference(*ID);
    if (!Ref) {
      Response->mutable_error()->set_description("cannot resolve refs");
      return Status::OK;
    }
    Refs.push_back(*Ref);
  }
  auto Handle = MockDB->store(Refs, Data);
  if (!Handle) {
    Response->mutable_error()->set_description(toString(Handle.takeError()));
    return Status::OK;
  }

  Response->mutable_cas_id()->CopyFrom(getCASDataID(MockDB->getID(*Handle)));
  return Status::OK;
}

Status MockCASImpl::Load(ServerContext *Context, const CASLoadRequest *Request,
                         CASLoadResponse *Response) {
  // Get by converting to a CASID string first, then fetch.
  auto CASID = getCASIDFromDataID(Request->cas_id(), *MockDB);
  if (!CASID) {
    Response->set_outcome(CASLoadResponse_Outcome_ERROR);
    Response->mutable_error()->set_description(toString(CASID.takeError()));
    return Status::OK;
  }
  auto Proxy = MockDB->getProxy(*CASID);
  if (!Proxy) {
    Response->set_outcome(CASLoadResponse_Outcome_ERROR);
    Response->mutable_error()->set_description(toString(Proxy.takeError()));
    return Status::OK;
  }
  Response->mutable_data()->mutable_blob()->set_data(Proxy->getData().str());
  Response->set_outcome(CASLoadResponse_Outcome_SUCCESS);
  return Status::OK;
}

Status MockCASImpl::Save(ServerContext *Context, const CASSaveRequest *Request,
                         CASSaveResponse *Response) {
  ArrayRef<char> Data =
      arrayRefFromStringRef<char>(Request->data().blob().data());
  auto Handle = MockDB->store({}, Data);
  if (!Handle) {
    Response->mutable_error()->set_description(toString(Handle.takeError()));
    return Status::OK;
  }

  Response->mutable_cas_id()->CopyFrom(getCASDataID(MockDB->getID(*Handle)));
  return Status::OK;
}

MockEnvImpl::MockEnvImpl(std::string Path) {
  std::string Address("unix:");
  Address += Path;
  std::mutex ServerFinishLock;
  std::condition_variable CVFinish;
  bool IsFinished = false;
  auto ServerFunc = [&, Address] {
    MockKeyValueImpl KVService;
    MockCASImpl CASService;

    ServerBuilder Builder;
    Builder.AddListeningPort(Address, InsecureServerCredentials());
    Builder.RegisterService(&KVService);
    Builder.RegisterService(&CASService);
    std::shared_ptr<Server> OwningServer(Builder.BuildAndStart());
    ServerRef = OwningServer;
    {
      std::lock_guard<std::mutex> FinishGuard(ServerFinishLock);
      IsFinished = true;
    }
    CVFinish.notify_all();
    OwningServer->Wait();
  };
  Thread = std::make_unique<thread>(ServerFunc);
  Thread->detach();
  // Wait till server created before return.
  std::unique_lock<std::mutex> FinishLock(ServerFinishLock);
  CVFinish.wait(FinishLock, [&](){ return IsFinished; });
}

MockEnvImpl::~MockEnvImpl() { ServerRef->Shutdown(); }

std::unique_ptr<llvm::unittest::cas::MockEnv> createGRPCEnv(StringRef Path) {
  return std::make_unique<MockEnvImpl>(Path.str());
}

#endif /* LLVM_ENABLE_GRPC_CAS */

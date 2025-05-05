#include "llvm/ADT/ScopeExit.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/BuiltinUnifiedCASDatabases.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/ThreadPool.h"

using namespace llvm;

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  static ExitOnError ExitOnErr("llvm-cas-fuzzer: ");

  if (Size == 0)
    return 0;

  SmallString<128> Path;
  if (sys::fs::createUniqueDirectory("CAS", Path) != std::error_code())
    return 1;

  ExitOnErr.setBanner(("llvm-cas-fuzzer using '" + Path + "':").str());

  size_t NumShards = std::max(uint8_t(1), std::min(Data[0], uint8_t(32)));
  ++Data;
  --Size;

  StringRef GlobalData(reinterpret_cast<const char *>(Data), Size);

  StdThreadPool ThreadPool(hardware_concurrency());

  for (size_t I = 0; I != NumShards; ++I) {
    ThreadPool.async([&, I] {
      auto [CAS, ActionCache] =
          ExitOnErr(cas::createOnDiskUnifiedCASDatabases(Path));

      ExitOnErr(CAS->setSizeLimit(128));

      StringRef LocalData =
          GlobalData.drop_front(std::min(I, GlobalData.size()));

      std::string KeySuffix = std::to_string(I);

      std::string KeyIDStr;
      {
        cas::ObjectRef DataRef = ExitOnErr(CAS->storeFromString({}, LocalData));
        cas::CASID DataID = CAS->getID(DataRef);

        cas::ObjectRef KeyPrefixRef =
            ExitOnErr(CAS->storeFromString({}, "key"));
        cas::ObjectRef KeySuffixRef =
            ExitOnErr(CAS->storeFromString({}, KeySuffix));
        cas::ObjectRef KeyRef =
            ExitOnErr(CAS->storeFromString({KeyPrefixRef, KeySuffixRef}, "_"));
        cas::CASID KeyID = CAS->getID(KeyRef);
        ExitOnErr(ActionCache->put(KeyID, DataID));
        KeyIDStr = KeyID.toString();
      }

      {
        cas::CASID KeyID = ExitOnErr(CAS->parseID(KeyIDStr));
        auto MaybeKeyRef = CAS->getReference(KeyID);
        assert(MaybeKeyRef);
        cas::ObjectProxy KeyProxy = ExitOnErr(CAS->getProxy(*MaybeKeyRef));
        assert(KeyProxy.getData() == "_");
        assert(KeyProxy.getNumReferences() == 2);
        cas::ObjectProxy KeyPrefixProxy =
            ExitOnErr(CAS->getProxy(KeyProxy.getReference(0)));
        assert(KeyPrefixProxy.getData() == "key");
        cas::ObjectProxy KeySuffixProxy =
            ExitOnErr(CAS->getProxy(KeyProxy.getReference(1)));
        assert(KeySuffixProxy.getData() == KeySuffix);

        auto MaybeDataID = ExitOnErr(ActionCache->get(KeyID));
        assert(MaybeDataID);
        cas::ObjectProxy DataProxy = ExitOnErr(CAS->getProxy(*MaybeDataID));
        assert(DataProxy.getData() == LocalData);
      }

      ExitOnErr(CAS->pruneStorageData());
    });
  }

  ThreadPool.wait();

  sys::fs::remove_directories(Path);

  return 0;
}

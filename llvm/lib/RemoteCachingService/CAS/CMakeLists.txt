set(LLVM_LINK_COMPONENTS
  RemoteCacheKVProto
  RemoteCacheCASProto
  RemoteCacheClient
  )

add_llvm_library(LLVMGRPCRelayCAS
  GRPCRelayCAS.cpp

  LINK_COMPONENTS
  CAS
)

generate_protos(LLVMRemoteCacheKVProto "compilation_caching_kv.proto" GRPC)
generate_protos(LLVMRemoteCacheCASProto "compilation_caching_cas.proto" GRPC)

set(LLVM_LINK_COMPONENTS
  RemoteCacheKVProto
  RemoteCacheCASProto
  )

include_directories($<TARGET_PROPERTY:LLVMRemoteCacheKVProto,BINARY_DIR>)
include_directories($<TARGET_PROPERTY:LLVMRemoteCacheCASProto,BINARY_DIR>)

set(SOURCES
  GRPCRelayCAS.cpp
  )

add_llvm_library(LLVMRemoteCachingService
  GRPCRelayCAS.cpp

  LINK_COMPONENTS
  CAS
)

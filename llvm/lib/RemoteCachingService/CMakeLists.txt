if (LLVM_ENABLE_GRPC_CAS)
  generate_protos(LLVMRemoteCachingKVProto "compilation_caching_kv.proto"
    GRPC)
  generate_protos(LLVMRemoteCachingCASProto "compilation_caching_cas.proto"
    GRPC)

  set(LLVM_LINK_COMPONENTS
    RemoteCachingKVProto
    RemoteCachingCASProto
    )

  include_directories($<TARGET_PROPERTY:LLVMRemoteCachingKVProto,BINARY_DIR>)
  include_directories($<TARGET_PROPERTY:LLVMRemoteCachingCASProto,BINARY_DIR>)
endif()

set(SOURCES
  GRPCRelayCAS.cpp
  )

add_llvm_library(LLVMRemoteCachingService
  GRPCRelayCAS.cpp

  LINK_COMPONENTS
  CAS
)

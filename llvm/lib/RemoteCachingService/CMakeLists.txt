if (LLVM_CAS_ENABLE_REMOTE_CACHE)
add_subdirectory(RemoteCacheProto)
add_subdirectory(RemoteCacheServer)

set(LLVM_LINK_COMPONENTS
  RemoteCacheKVProto
  RemoteCacheCASProto
  )

  include_directories($<TARGET_PROPERTY:LLVMRemoteCacheKVProto,BINARY_DIR>)
  include_directories($<TARGET_PROPERTY:LLVMRemoteCacheCASProto,BINARY_DIR>)

  add_llvm_component_library(LLVMRemoteCachingService
  RemoteCachingService.cpp
  Client/Client.cpp
  CAS/GRPCRelayCAS.cpp

  LINK_COMPONENTS
  Support
  CAS
)

else ()
add_llvm_component_library(LLVMRemoteCachingService
  RemoteCachingService.cpp
  NullService/NullService.cpp

  LINK_COMPONENTS
  Support
  CAS
)
endif()

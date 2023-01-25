if (LLVM_CAS_ENABLE_REMOTE_CACHE)
add_subdirectory(RemoteCacheProto)
add_subdirectory(RemoteCacheServer)

set(LLVM_LINK_COMPONENTS
  RemoteCacheKVProto
  RemoteCacheCASProto
)

include_directories($<TARGET_PROPERTY:LLVMRemoteCacheKVProto,BINARY_DIR>)
include_directories($<TARGET_PROPERTY:LLVMRemoteCacheCASProto,BINARY_DIR>)

set(REMOTE_CACHE_SOURCE 
  Client/Client.cpp 
  CAS/GRPCRelayCAS.cpp
)

else ()
set(REMOTE_CACHE_SOURCE NullService/NullService.cpp)
endif()

add_llvm_component_library(LLVMRemoteCachingService
 ${REMOTE_CACHE_SOURCE}
 RemoteCachingService.cpp

 LINK_COMPONENTS
  Support
  CAS

)
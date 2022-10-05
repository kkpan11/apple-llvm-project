include_directories($<TARGET_PROPERTY:LLVMRemoteCacheKVProto,BINARY_DIR>)
include_directories($<TARGET_PROPERTY:LLVMRemoteCacheCASProto,BINARY_DIR>)

set(LLVM_LINK_COMPONENTS
  RemoteCacheCASProto
  RemoteCacheKVProto
)

add_clang_library(clangRemoteCache
  Client.cpp
)

if (TARGET obj.clangRemoteCache)
  # Needed so that the grpc headers are generated ahead of compiling the files of the library.
  add_dependencies(obj.clangRemoteCache
    LLVMRemoteCacheCASProto
    LLVMRemoteCacheKVProto
  )
endif()

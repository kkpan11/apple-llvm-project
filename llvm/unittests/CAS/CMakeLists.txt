set(LLVM_LINK_COMPONENTS
  Support
  CAS
  TestingSupport
  )

add_llvm_unittest(CASTests
  ActionCacheTest.cpp
  CASFileSystemTest.cpp
  CASTestConfig.cpp
  CASOutputBackendTest.cpp
  CASProvidingFileSystemTest.cpp
  CachingOnDiskFileSystemTest.cpp
  LazyAtomicPointerTest.cpp
  HashMappedTrieTest.cpp
  HierarchicalTreeBuilderTest.cpp
  MockGRPCServer.cpp
  ObjectStoreTest.cpp
  OnDiskHashMappedTrieTest.cpp
  ThreadSafeAllocatorTest.cpp
  TreeSchemaTest.cpp
  )

target_link_libraries(CASTests PRIVATE LLVMTestingSupport)

if(LLVM_ENABLE_GRPC_CAS)
  include_directories($<TARGET_PROPERTY:LLVMRemoteCachingKVProto,BINARY_DIR>)
  include_directories($<TARGET_PROPERTY:LLVMRemoteCachingCASProto,BINARY_DIR>)
  target_link_libraries(CASTests PRIVATE LLVMRemoteCachingService)
endif()

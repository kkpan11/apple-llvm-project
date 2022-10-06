if (LLVM_CAS_ENABLE_REMOTE_CACHE)
  append("-DLLVM_CAS_ENABLE_REMOTE_CACHE=1" CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
  set(ADDITIONAL_CAS_TEST_DEPS "RemoteCacheServer")
endif()

set(LLVM_LINK_COMPONENTS
  Support
  CAS
  RemoteCachingService
  TestingSupport
  ${ADDITIONAL_CAS_TEST_DEPS}
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

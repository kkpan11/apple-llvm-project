add_subdirectory(Breakpad)
add_subdirectory(ELF)
add_subdirectory(MachO)
if (NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
  # PECOFF/TestSectionSize.cpp uses a SizeOfRawData key not recognized
  # by the yaml2pecoff on this branch on darwin; test fails to build.
  add_subdirectory(PECOFF)
endif()


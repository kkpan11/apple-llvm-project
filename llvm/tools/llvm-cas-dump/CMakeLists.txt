set(LLVM_LINK_COMPONENTS
  Support
  CAS
  CASObjectFormats
  )

add_llvm_tool(llvm-cas-dump
  llvm-cas-dump.cpp
  MCCASPrinter.cpp
)

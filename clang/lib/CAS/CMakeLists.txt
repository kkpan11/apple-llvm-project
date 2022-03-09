set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangCAS
  CASOptions.cpp

  LINK_LIBS
  clangBasic
  )


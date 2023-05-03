set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  CodeGen
  CodeGenTypes
  Core
  MC
  MCCAS
  MIRParser
  Support
  Target
  TargetParser
  )

add_llvm_unittest(MITests
  LiveIntervalTest.cpp
  )

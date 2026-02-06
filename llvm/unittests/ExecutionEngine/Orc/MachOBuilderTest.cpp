//===---------- MachOBuilderTest.cpp - MachOBuilder Tests -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/MachOBuilder.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::orc;

static Expected<std::unique_ptr<object::MachOObjectFile>>
parseMachO(ArrayRef<char> Buffer) {
  return object::ObjectFile::createMachOObjectFile(
      MemoryBufferRef(StringRef(Buffer.data(), Buffer.size()), "test"));
}

TEST(MachOBuilderTest, AddLCTargetTriple) {
  std::string TestTriple = "x86_64-apple-darwin";
  MachOBuilder<MachO64LE> B(4096);
  B.Header.filetype = MachO::MH_OBJECT;

  B.addLoadCommand<MachO::LC_TARGET_TRIPLE>(TestTriple);

  size_t Size = B.layout();
  std::vector<char> Buffer(Size, 0);
  B.write({Buffer.data(), Buffer.size()});

  auto Obj = parseMachO(Buffer);
  ASSERT_THAT_EXPECTED(Obj, Succeeded());

  bool Found = false;
  for (auto &LC : (*Obj)->load_commands()) {
    if (LC.C.cmd == MachO::LC_TARGET_TRIPLE) {
      Found = true;
      MachO::target_triple_command TTC = (*Obj)->getTargetTripleLoadCommand(LC);
      ASSERT_LT(TTC.triple, TTC.cmdsize);
      EXPECT_EQ(LC.Ptr + TTC.triple, TestTriple);
      break;
    }
  }

  EXPECT_TRUE(Found);
}

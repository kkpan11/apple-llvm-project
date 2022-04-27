//===- CASReferenceTest.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASReference.h"
#include "llvm/Testing/Support/SupportHelpers.h"
#include "gtest/gtest.h"

namespace llvm {
namespace cas {
namespace testing_helpers {
class HandleFactory {
public:
  template <class HandleT> static HandleT make(uint64_t InternalRef = 0) {
    return HandleT(/*CAS=*/nullptr, InternalRef, /*IsHandle=*/true);
  }
};
} // end namespace testing_helpers
} // end namespace cas
} // end namespace llvm

using namespace llvm;
using namespace llvm::cas;
using namespace llvm::cas::testing_helpers;

namespace {

TEST(CASReferenceTest, BlobHandle) {
  static_assert(std::is_convertible<BlobHandle, AnyObjectHandle>(), "");
  static_assert(!std::is_constructible<AnyDataHandle, BlobHandle>(), "");
  static_assert(!std::is_constructible<TreeHandle, BlobHandle>(), "");
  static_assert(!std::is_constructible<NodeHandle, BlobHandle>(), "");
  static_assert(!std::is_constructible<ObjectRef, BlobHandle>(), "");
  static_assert(!std::is_constructible<BlobHandle, ObjectRef>(), "");

  BlobHandle Blob = HandleFactory::make<BlobHandle>();
  AnyObjectHandle AnyObject = Blob;
  ASSERT_TRUE(AnyObject.is<BlobHandle>());
  ASSERT_EQ(Blob, AnyObject.get<BlobHandle>());
  ASSERT_TRUE(AnyObject.is<BlobHandle>());

  AnyDataHandle AnyData = Blob.getData();
  ASSERT_EQ(Blob, AnyData.get<BlobHandle>());

  ASSERT_TRUE(AnyObject.getData());
  AnyData = *AnyObject.getData();
  ASSERT_TRUE(AnyData.is<BlobHandle>());
}

TEST(CASReferenceTest, TreeHandle) {
  static_assert(std::is_convertible<TreeHandle, AnyObjectHandle>(), "");
  static_assert(!std::is_constructible<AnyDataHandle, TreeHandle>(), "");
  static_assert(!std::is_constructible<BlobHandle, TreeHandle>(), "");
  static_assert(!std::is_constructible<NodeHandle, TreeHandle>(), "");
  static_assert(!std::is_constructible<ObjectRef, TreeHandle>(), "");
  static_assert(!std::is_constructible<TreeHandle, ObjectRef>(), "");

  TreeHandle Tree = HandleFactory::make<TreeHandle>();

  AnyObjectHandle AnyObject = Tree;
  ASSERT_TRUE(AnyObject.is<TreeHandle>());
  ASSERT_EQ(Tree, AnyObject.get<TreeHandle>());
}

TEST(CASReferenceTest, NodeHandle) {
  static_assert(std::is_convertible<NodeHandle, AnyObjectHandle>(), "");
  static_assert(!std::is_constructible<AnyDataHandle, NodeHandle>(), "");
  static_assert(!std::is_constructible<BlobHandle, NodeHandle>(), "");
  static_assert(!std::is_constructible<TreeHandle, NodeHandle>(), "");
  static_assert(!std::is_constructible<ObjectRef, NodeHandle>(), "");
  static_assert(!std::is_constructible<NodeHandle, ObjectRef>(), "");

  NodeHandle Node = HandleFactory::make<NodeHandle>();
  AnyObjectHandle AnyObject = Node;
  ASSERT_TRUE(AnyObject.is<NodeHandle>());
  ASSERT_EQ(Node, AnyObject.get<NodeHandle>());
  ASSERT_TRUE(AnyObject.is<NodeHandle>());

  AnyDataHandle AnyData = Node.getData();
  ASSERT_EQ(Node, AnyData.get<NodeHandle>());

  ASSERT_TRUE(AnyObject.getData());
  AnyData = *AnyObject.getData();
  ASSERT_TRUE(AnyData.is<NodeHandle>());
}

} // end namespace

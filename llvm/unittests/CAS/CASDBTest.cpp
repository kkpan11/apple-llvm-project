//===- CASDBTest.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASDB.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Testing/Support/SupportHelpers.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::cas;

struct TestingAndDir {
  std::unique_ptr<CASDB> DB;
  Optional<unittest::TempDir> Temp;
};

class CASDBTest
    : public testing::TestWithParam<std::function<TestingAndDir(int)>> {
protected:
  Optional<int> NextCASIndex;

  SmallVector<unittest::TempDir> Dirs;

  std::unique_ptr<CASDB> createCAS() {
    auto TD = GetParam()((*NextCASIndex)++);
    if (TD.Temp)
      Dirs.push_back(std::move(*TD.Temp));
    return std::move(TD.DB);
  }
  void SetUp() { NextCASIndex = 0; }
  void TearDown() {
    NextCASIndex = None;
    Dirs.clear();
  }
};

TEST_P(CASDBTest, Blobs) {
  std::unique_ptr<CASDB> CAS1 = createCAS();
  StringRef ContentStrings[] = {
      "word",
      "some longer text std::string's local memory",
      R"(multiline text multiline text multiline text multiline text
multiline text multiline text multiline text multiline text multiline text
multiline text multiline text multiline text multiline text multiline text
multiline text multiline text multiline text multiline text multiline text
multiline text multiline text multiline text multiline text multiline text
multiline text multiline text multiline text multiline text multiline text)",
  };

  ExitOnError ExitOnErr("Blobs");

  SmallVector<CASID> IDs;
  for (StringRef Content : ContentStrings) {
    // Use StringRef::str() to create a temporary std::string. This could cause
    // problems if the CAS is storing references to the input string instead of
    // copying it.
    IDs.push_back(ExitOnErr(CAS1->createBlob(Content)));
  }

  // Check that the blobs give the same IDs later.
  for (int I = 0, E = IDs.size(); I != E; ++I)
    EXPECT_EQ(IDs[I], ExitOnErr(CAS1->createBlob(ContentStrings[I])));

  // Check that the blobs can be retrieved multiple times.
  for (int I = 0, E = IDs.size(); I != E; ++I) {
    for (int J = 0, JE = 3; J != JE; ++J) {
      Optional<BlobRef> Buffer;
      ASSERT_THAT_ERROR(CAS1->getBlob(IDs[I]).moveInto(Buffer), Succeeded());
      EXPECT_EQ(ContentStrings[I], **Buffer);
    }
  }

  // Confirm these blobs don't exist in a fresh CAS instance.
  std::unique_ptr<CASDB> CAS2 = createCAS();
  for (int I = 0, E = IDs.size(); I != E; ++I)
    EXPECT_EQ(None, expectedToOptional(CAS2->getBlob(IDs[I])));

  // Insert into the second CAS and confirm the IDs are stable. Getting them
  // should work now.
  for (int I = IDs.size(), E = 0; I != E; --I) {
    auto &ID = IDs[I - 1];
    auto &Content = ContentStrings[I - 1];
    EXPECT_EQ(ID, ExitOnErr(CAS2->createBlob(Content)));

    Optional<BlobRef> Buffer;
    ASSERT_THAT_ERROR(CAS2->getBlob(ID).moveInto(Buffer), Succeeded());
    EXPECT_EQ(Content, **Buffer);
  }
}

TEST_P(CASDBTest, BlobsBig) {
  // A little bit of validation that bigger blobs are okay. Climb up to 1MB.
  std::unique_ptr<CASDB> CAS = createCAS();
  SmallString<256> String1 = StringRef("a few words");
  SmallString<256> String2 = StringRef("others");
  while (String1.size() < 1024U * 1024U) {
    Optional<CASID> ID1;
    Optional<CASID> ID2;
    ASSERT_THAT_ERROR(CAS->createBlob(String1).moveInto(ID1), Succeeded());
    ASSERT_THAT_ERROR(CAS->createBlob(String1).moveInto(ID2), Succeeded());
    ASSERT_EQ(ID1, ID2);
    String1.append(String2);
    ASSERT_THAT_ERROR(CAS->createBlob(String2).moveInto(ID1), Succeeded());
    ASSERT_THAT_ERROR(CAS->createBlob(String2).moveInto(ID2), Succeeded());
    ASSERT_EQ(ID1, ID2);
    String2.append(String1);
  }
}

TEST_P(CASDBTest, Trees) {
  std::unique_ptr<CASDB> CAS1 = createCAS();
  std::unique_ptr<CASDB> CAS2 = createCAS();

  ExitOnError ExitOnErr("Trees: ");

  auto createBlobInBoth = [&](StringRef Content) {
    Optional<CASID> ID1, ID2;
    EXPECT_THAT_ERROR(CAS1->createBlob(Content).moveInto(ID1), Succeeded());
    EXPECT_THAT_ERROR(CAS2->createBlob(Content).moveInto(ID2), Succeeded());
    EXPECT_EQ(ID1, ID2);
    return *ID1;
  };

  CASID Blob1 = createBlobInBoth("blob1");
  CASID Blob2 = createBlobInBoth("blob2");
  CASID Blob3 = createBlobInBoth("blob3");

  SmallVector<SmallVector<NamedTreeEntry, 0>, 0> FlatTreeEntries = {
      {},
      {NamedTreeEntry(Blob1, TreeEntry::Regular, "regular")},
      {NamedTreeEntry(Blob2, TreeEntry::Executable, "executable")},
      {NamedTreeEntry(Blob3, TreeEntry::Symlink, "symlink")},
      {
          NamedTreeEntry(Blob1, TreeEntry::Regular, "various"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "names"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "that"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "do"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "not"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "conflict"),
          NamedTreeEntry(Blob1, TreeEntry::Regular, "but have spaces and..."),
          NamedTreeEntry(Blob1, TreeEntry::Regular,
                         "`~,!@#$%^&*()-+=[]{}\\<>'\""),
      },
  };

  SmallVector<CASID> FlatIDs;
  for (ArrayRef<NamedTreeEntry> Entries : FlatTreeEntries) {
    Optional<CASID> ID;
    ASSERT_THAT_ERROR(CAS1->createTree(Entries).moveInto(ID), Succeeded());
    FlatIDs.push_back(*ID);
  }

  // Confirm we get the same IDs the second time and that the trees can be
  // visited (the entries themselves will be checked later).
  for (int I = 0, E = FlatIDs.size(); I != E; ++I) {
    Optional<CASID> ID;
    ASSERT_THAT_ERROR(CAS1->createTree(FlatTreeEntries[I]).moveInto(ID),
                      Succeeded());
    EXPECT_EQ(FlatIDs[I], ID);
    Optional<TreeRef> Tree;
    ASSERT_THAT_ERROR(CAS1->getTree(FlatIDs[I]).moveInto(Tree), Succeeded());
    EXPECT_EQ(FlatTreeEntries[I].size(), Tree->size());

    size_t NumCalls = 0;
    EXPECT_FALSE(
        errorToBool(Tree->forEachEntry([&NumCalls](const NamedTreeEntry &E) {
          ++NumCalls;
          return Error::success();
        })));
    EXPECT_EQ(FlatTreeEntries[I].size(), NumCalls);
  }

  // Confirm these trees don't exist in a fresh CAS instance. Skip the first
  // tree, which is empty and could be implicitly in some CAS.
  for (int I = 1, E = FlatIDs.size(); I != E; ++I)
    ASSERT_THAT_EXPECTED(CAS2->getTree(FlatIDs[I]), Failed());

  // Insert into the other CAS and confirm the IDs are stable.
  for (int I = FlatIDs.size(), E = 0; I != E; --I) {
    auto &ID = FlatIDs[I - 1];
    // Make a copy of the original entries and sort them.
    SmallVector<NamedTreeEntry, 0> SortedEntries = FlatTreeEntries[I - 1];
    llvm::sort(SortedEntries);

    // Confirm we get the same tree out of CAS2.
    EXPECT_EQ(ID, ExitOnErr(CAS2->createTree(SortedEntries)));

    // Check that the correct entries come back.
    for (CASDB *CAS : {&*CAS1, &*CAS2}) {
      Optional<TreeRef> Tree = expectedToOptional(CAS->getTree(ID));
      EXPECT_TRUE(Tree);
      for (int I = 0, E = SortedEntries.size(); I != E; ++I)
        EXPECT_EQ(SortedEntries[I], Tree->get(I));
    }
  }

  // Create some nested trees.
  SmallVector<CASID> NestedTrees = FlatIDs;
  for (int I = 0, E = FlatTreeEntries.size() * 3; I != E; ++I) {
    // Copy one of the flat entries and add some trees.
    auto OriginalEntries =
        makeArrayRef(FlatTreeEntries[I % FlatTreeEntries.size()]);
    SmallVector<NamedTreeEntry> Entries(OriginalEntries.begin(),
                                        OriginalEntries.end());
    std::string Name = ("tree" + Twine(I)).str();
    Entries.emplace_back(FlatIDs[(I + 4) % FlatIDs.size()], TreeEntry::Tree,
                         Name);

    Optional<std::string> Name1, Name2;
    if (NestedTrees.size() >= 2) {
      int Nested1 = I % NestedTrees.size();
      int Nested2 = (I * 3 + 2) % NestedTrees.size();
      if (Nested2 == Nested1)
        Nested2 = (Nested1 + 1) % NestedTrees.size();
      ASSERT_NE(Nested1, Nested2);
      Name1.emplace(("tree" + Twine(I) + "-" + Twine(Nested1)).str());
      Name2.emplace(("tree" + Twine(I) + "-" + Twine(Nested2)).str());

      Entries.emplace_back(NestedTrees[I % NestedTrees.size()], TreeEntry::Tree,
                           *Name1);
      Entries.emplace_back(NestedTrees[(I * 3 + 2) % NestedTrees.size()],
                           TreeEntry::Tree, *Name2);
    }
    CASID ID = ExitOnErr(CAS1->createTree(Entries));
    llvm::sort(Entries);
    EXPECT_EQ(ID, ExitOnErr(CAS1->createTree(Entries)));
    EXPECT_EQ(ID, ExitOnErr(CAS2->createTree(Entries)));

    for (CASDB *CAS : {&*CAS1, &*CAS2}) {
      Optional<TreeRef> Tree;
      ASSERT_THAT_ERROR(CAS->getTree(ID).moveInto(Tree), Succeeded());
      for (int I = 0, E = Entries.size(); I != E; ++I)
        EXPECT_EQ(Entries[I], Tree->get(I));
    }
  }
}

INSTANTIATE_TEST_SUITE_P(InMemoryCAS, CASDBTest, ::testing::Values([](int) {
                           return TestingAndDir{createInMemoryCAS(), None};
                         }));
static TestingAndDir createOnDisk(int I) {
  unittest::TempDir Temp("on-disk-cas", /*Unique=*/true);
  std::unique_ptr<CASDB> CAS;
  EXPECT_THAT_ERROR(createOnDiskCAS(Temp.path()).moveInto(CAS), Succeeded());
  return TestingAndDir{std::move(CAS), std::move(Temp)};
}
INSTANTIATE_TEST_SUITE_P(OnDiskCAS, CASDBTest, ::testing::Values(createOnDisk));

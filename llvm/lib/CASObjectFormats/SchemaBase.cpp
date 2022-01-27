//===- Schema.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CASObjectFormats/SchemaBase.h"
#include "llvm/CASObjectFormats/FlatV1.h"
#include "llvm/CASObjectFormats/NestedV1.h"

using namespace llvm;
using namespace llvm::casobjectformats;

void SchemaBase::anchor() {}

SchemaPool::SchemaPool(cas::CASDB &CAS) {
  Schemas.push_back(std::make_unique<flatv1::ObjectFileSchema>(CAS));
  Schemas.push_back(std::make_unique<nestedv1::ObjectFileSchema>(CAS));
}

SchemaBase *SchemaPool::getSchemaForRoot(cas::NodeRef Node) const {
  for (auto &Schema : Schemas) {
    if (Schema->isRootNode(Node))
      return Schema.get();
  }
  return nullptr;
}

Expected<std::unique_ptr<reader::CASObjectReader>>
SchemaPool::createObjectReader(cas::CASID ID) const {
  Expected<cas::NodeRef> Ref = getCAS().getNode(ID);
  if (auto E = Ref.takeError())
    return std::move(E);
  SchemaBase *Schema = getSchemaForRoot(*Ref);
  if (!Schema)
    return createStringError(inconvertibleErrorCode(),
                             "CAS object is not a recognized object file");
  return Schema->createObjectReader(*Ref);
}

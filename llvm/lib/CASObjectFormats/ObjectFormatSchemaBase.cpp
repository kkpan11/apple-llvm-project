//===- ObjectFormatSchemaBase.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CASObjectFormats/ObjectFormatSchemaBase.h"
#include "llvm/CASObjectFormats/FlatV1.h"
#include "llvm/CASObjectFormats/NestedV1.h"

using namespace llvm;
using namespace llvm::casobjectformats;

char ObjectFormatSchemaBase::ID = 0;
void ObjectFormatSchemaBase::anchor() {}

void casobjectformats::addObjectFormatSchemas(cas::SchemaPool &Pool) {
  auto &CAS = Pool.getCAS();
  Pool.addSchema(std::make_unique<flatv1::ObjectFileSchema>(CAS));
  Pool.addSchema(std::make_unique<nestedv1::ObjectFileSchema>(CAS));
}

Expected<std::unique_ptr<reader::CASObjectReader>>
casobjectformats::createObjectReader(const cas::SchemaPool &Pool,
                                     cas::CASID ID) {
  Expected<cas::NodeProxy> Ref = Pool.getCAS().getNode(ID);
  if (auto E = Ref.takeError())
    return std::move(E);
  if (auto *Schema =
          dyn_cast_or_null<ObjectFormatSchemaBase>(Pool.getSchemaForRoot(*Ref)))
    return Schema->createObjectReader(*Ref);
  return createStringError(inconvertibleErrorCode(),
                           "CAS object is not a recognized object file");
}

Expected<std::unique_ptr<reader::CASObjectReader>>
ObjectFormatSchemaPool::createObjectReader(cas::CASID ID) const {
  return casobjectformats::createObjectReader(Pool, ID);
}

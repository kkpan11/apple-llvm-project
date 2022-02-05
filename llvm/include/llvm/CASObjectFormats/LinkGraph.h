//===- llvm/CASObjectFormats/LinkGraph.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CASOBJECTFORMATS_LINKGRAPH_H
#define LLVM_CASOBJECTFORMATS_LINKGRAPH_H

#include "llvm/Support/Error.h"
#include <memory>

namespace llvm {
namespace jitlink {
class LinkGraph;
}

namespace casobjectformats {
namespace reader {
class CASObjectReader;
}

/// Eagerly parse the full compile unit to create a LinkGraph.
///
/// Ideally we'd have some sort of LazyLinkGraph that can answer questions
/// and/or build itself up on demand.
Expected<std::unique_ptr<jitlink::LinkGraph>>
createLinkGraph(const reader::CASObjectReader &Reader, StringRef Name);

} // end namespace casobjectformats
} // end namespace llvm

#endif // LLVM_CASOBJECTFORMATS_LINKGRAPH_H

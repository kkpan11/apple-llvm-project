//===- llvm/CASObjectFormats/Utils.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CASOBJECTFORMATS_UTILS_H
#define LLVM_CASOBJECTFORMATS_UTILS_H

namespace llvm {

class Error;
class raw_ostream;

namespace casobjectformats {

namespace reader {
class CASObjectReader;
}

Error printCASObject(const reader::CASObjectReader &Reader, raw_ostream &OS);

} // end namespace casobjectformats
} // end namespace llvm

#endif // LLVM_CASOBJECTFORMATS_UTILS_H

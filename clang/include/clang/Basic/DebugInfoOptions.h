//===--- DebugInfoOptions.h - Debug Info Emission Types ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_DEBUGINFOOPTIONS_H
#define LLVM_CLANG_BASIC_DEBUGINFOOPTIONS_H

namespace clang {
namespace codegenoptions {

enum DebugInfoFormat {
  DIF_DWARF,
  DIF_CodeView,
};

enum CasFriendlinessKind {
  NoCasFriendlyDebugInfo,

  /// Generate CAS friendly debug info to go along with the work being done for
  /// llvm-cas. This option will split the line tables so that every function
  /// has its own line table header. Note: To achieve this, every function is
  /// given its own compile unit.
  DebugLineOnly,

  /// Generate CAS friendly debug info to go along with the work being done for
  /// llvm-cas. This option will make it so that every compiler unit has its own
  /// contribution into the debug abbreviation section. Note: To achieve this,
  /// every function is given its own compile unit, which means each compile
  /// unit also has its own line table header.
  DebugAbbrev
};

enum DebugInfoKind {
  /// Don't generate debug info.
  NoDebugInfo,

  /// Emit location information but do not generate debug info in the output.
  /// This is useful in cases where the backend wants to track source
  /// locations for instructions without actually emitting debug info for them
  /// (e.g., when -Rpass is used).
  LocTrackingOnly,

  /// Emit only debug directives with the line numbers data
  DebugDirectivesOnly,

  /// Emit only debug info necessary for generating line number tables
  /// (-gline-tables-only).
  DebugLineTablesOnly,

  /// Limit generated debug info to reduce size (-fno-standalone-debug). This
  /// emits forward decls for types that could be replaced with forward decls in
  /// the source code. For dynamic C++ classes type info is only emitted into
  /// the module that contains the classe's vtable.
  LimitedDebugInfo,

  /// Generate complete debug info.
  FullDebugInfo,

  /// Generate debug info for types that may be unused in the source
  /// (-fno-eliminate-unused-debug-types).
  UnusedTypeInfo,
};

enum class DebugTemplateNamesKind {
  Full,
  Simple,
  Mangled
};

} // end namespace codegenoptions
} // end namespace clang

#endif

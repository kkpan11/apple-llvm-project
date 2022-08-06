//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CASDWARFObject.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAbbrev.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/FormatVariadic.h"

using namespace llvm;
using namespace llvm::cas;
using namespace llvm::mccasformats::v1;

namespace {
/// Parse the MachO header to extract details such as endianness.
/// Unfortunately object::MachOObjectfile() doesn't support parsing
/// incomplete files.
struct MachOHeaderParser {
  bool Is64Bit = true;
  bool IsLittleEndian = true;
  uint32_t DebugAbbrevOffset = 0;
  uint64_t DebugAbbrevSize = 0;

  /// Stolen from MachOObjectfile.
  template <typename T>
  Expected<T> getStructOrErr(StringRef Data, const char *P) {
    // Don't read before the beginning or past the end of the file
    if (P < Data.begin() || P + sizeof(T) > Data.end())
      return make_error<llvm::object::GenericBinaryError>(
          "Structure read out-of-range");

    T Cmd;
    memcpy(&Cmd, P, sizeof(T));
    if (IsLittleEndian != sys::IsLittleEndianHost)
      MachO::swapStruct(Cmd);
    return Cmd;
  }

  /// Parse an mc::header.
  Error parse(StringRef Data) {
    // MachO 64-bit header.
    const char *P = Data.data();
    auto Header64 = getStructOrErr<MachO::mach_header_64>(Data, P);
    P += sizeof(MachO::mach_header_64);
    if (!Header64)
      return Header64.takeError();
    if (Header64->magic == MachO::MH_MAGIC_64) {
      Is64Bit = true;
      IsLittleEndian = true;
    } else {
      return make_error<object::GenericBinaryError>("Unsupported MachO format");
    }
    // Iterate over load commands to find the __debug_abbrev section.
    // This code can be removed once that section gets its own block kind.
    for (unsigned I = 0; I < Header64->ncmds; ++I) {
      auto LoadCmd = getStructOrErr<MachO::load_command>(Data, P);
      if (!LoadCmd)
        return LoadCmd.takeError();
      if (LoadCmd->cmd != MachO::LC_SEGMENT_64) {
        P += LoadCmd->cmdsize;
        continue;
      }
      auto SegCmd = getStructOrErr<MachO::segment_command_64>(Data, P);
      if (!SegCmd)
        return SegCmd.takeError();
      P += sizeof(MachO::segment_command_64);
      for (unsigned J = 0; J < SegCmd->nsects; ++J) {
        auto Section = getStructOrErr<MachO::section_64>(Data, P);
        if (!Section)
          return Section.takeError();
        P += sizeof(MachO::section_64);
        if (StringRef(Section->sectname,
                      strnlen(Section->sectname, sizeof(Section->sectname))) ==
            "__debug_abbrev") {
          DebugAbbrevOffset = Section->offset - Data.size();
          DebugAbbrevSize = Section->size;
          return Error::success();
        }
      }
    }
    return Error::success();
  }
};
} // namespace

Error CASDWARFObject::discoverDwarfSections(ObjectRef CASObj) {
  if (CASObj == Schema.getRootNodeTypeID())
    return Error::success();
  Expected<MCObjectProxy> MCObj = Schema.get(CASObj);
  if (!MCObj)
    return MCObj.takeError();
  return discoverDwarfSections(*MCObj);
}

Error CASDWARFObject::discoverDwarfSections(MCObjectProxy MCObj) {
  StringRef Data = MCObj.getData();
  if (HeaderRef::Cast(MCObj)) {
    MachOHeaderParser P;
    if (Error Err = P.parse(MCObj.getData()))
      return Err;
    Is64Bit = P.Is64Bit;
    IsLittleEndian = P.IsLittleEndian;
    DebugAbbrevOffset = P.DebugAbbrevOffset;
    DebugAbbrevSize = P.DebugAbbrevSize;
  }
  if (DebugAbbrevSection.empty() &&
      MCObj.getKindString().startswith("mc:data")) {
    StringRef Data = MCObj.getData();
    if (DebugAbbrevOffset < Data.size()) {
      DebugAbbrevSection =
          Data.drop_front(DebugAbbrevOffset).take_front(DebugAbbrevSize);
    } else {
      DebugAbbrevOffset -= Data.size();
    }
  }

  if (DebugStrRef::Cast(MCObj)) {
    DebugStringSection.append(Data.begin(), Data.end());
    DebugStringSection.push_back(0);
  }
  return MCObj.forEachReference(
      [this](ObjectRef CASObj) { return discoverDwarfSections(CASObj); });
}

Error CASDWARFObject::dump(raw_ostream &OS, int Indent, DWARFContext &DWARFCtx,
                           MCObjectProxy MCObj) const {
  OS.indent(Indent);
  DIDumpOptions DumpOpts;
  Error Err = Error::success();
  StringRef Data = MCObj.getData();
  if (Data.empty())
    return Err;
  if (DebugStrRef::Cast(MCObj)) {
    // Dump __debug_str data.
    assert(Data.data()[Data.size()] == 0);
    DataExtractor StrData(StringRef(Data.data(), Data.size() + 1),
                          isLittleEndian(), 0);
    // This is almost identical with the DumpStrSection lambda in
    // DWARFContext.cpp
    uint64_t Offset = 0;
    uint64_t StrOffset = 0;
    while (StrData.isValidOffset(Offset)) {
      const char *CStr = StrData.getCStr(&Offset, &Err);
      if (Err)
        return Err;
      OS << format("0x%8.8" PRIx64 ": \"", StrOffset);
      OS.write_escaped(CStr);
      OS << "\"\n";
      StrOffset = Offset;
    }
  } else if (DebugLineRef::Cast(MCObj)) {
    // Dump __debug_line data.
    uint64_t Address = 0;
    DWARFDataExtractor LineData(*this, {Data, Address}, isLittleEndian(), 0);
    DWARFDebugLine::SectionParser Parser(LineData, DWARFCtx,
                                         DWARFCtx.normal_units());
    while (!Parser.done()) {
      OS << "debug_line[" << format("0x%8.8" PRIx64, Parser.getOffset())
         << "]\n";
      Parser.parseNext(DumpOpts.WarningHandler, DumpOpts.WarningHandler, &OS,
                       DumpOpts.Verbose);
    }
  } else if (DebugInfoCURef::Cast(MCObj)) {
    // Dump __debug_info data.
    DWARFUnitVector UV;
    uint64_t Address = 0;
    DWARFSection Section = {Data, Address};
    DWARFUnitHeader Header;
    DWARFDebugAbbrev Abbrev;
    Abbrev.extract(
        DataExtractor(getAbbrevSection(), isLittleEndian(), getAddressSize()));
    uint64_t offset_ptr = 0;
    Header.extract(
        DWARFCtx,
        DWARFDataExtractor(*this, Section, isLittleEndian(), getAddressSize()),
        &offset_ptr, DWARFSectionKind::DW_SECT_INFO);
    DWARFCompileUnit U(DWARFCtx, Section, Header, &Abbrev, &getRangesSection(),
                       &getLocSection(), getStrSection(),
                       getStrOffsetsSection(), &getAddrSection(),
                       getLocSection(), isLittleEndian(), false, UV);
    U.dump(OS, DumpOpts);
    // TODO: Dump more than just the CU.
  }
  return Err;
}

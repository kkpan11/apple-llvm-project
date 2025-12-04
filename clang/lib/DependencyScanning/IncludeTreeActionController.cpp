//===- IncludeTreeActionController.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/APINotes/APINotesManager.h"
#include "clang/APINotes/APINotesReader.h"
#include "clang/Basic/DiagnosticCAS.h"
#include "clang/CAS/IncludeTree.h"
#include "clang/DependencyScanning/CachingActions.h"
#include "clang/DependencyScanning/ScanAndUpdateArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/CAS/CASFileSystem.h"
#include "llvm/CAS/CASProvidingFileSystem.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/PrefixMapper.h"
#include "llvm/Support/PrefixMappingFileSystem.h"

using namespace clang;
using namespace dependencies;
using llvm::Error;

#define PROPAGATE_FALSE(EXPR)                                                  \
  if (!(EXPR))                                                                 \
    return false;

static void writeBool(bool Val, SmallString<128> &Data) {
  Data += Val ? '\1' : '\0';
}

static bool readBool(bool &Val, StringRef Data) {
  if (Data.empty())
    return false;
  if (Data[0] == '\1')
    Val = true;
  else if (Data[0] == '\0')
    Val = false;
  else
    return false;
  return true;
}

static void writeString(StringRef Str, SmallString<128> &Data) {
  Data += Str;
  Data += '\0';
}

static bool readString(std::string &Str, StringRef &Data) {
  size_t StrEnd = Data.find('\0');
  if (StrEnd == StringRef::npos || StrEnd == 0)
    return false;
  Str = Data.substr(0, StrEnd);
  Data = Data.substr(StrEnd + 1);
  return true;
}

static void writeNull(SmallString<128> &Data) { Data += '\0'; }

static bool readNull(StringRef &Data) {
  if (Data.empty() || Data[0] != '\0')
    return false;
  Data = Data.substr(1);
  return true;
}

static void writeP1689ModuleInfo(const P1689ModuleInfo &Info,
                                 SmallString<128> &Data) {
  writeString(Info.ModuleName, Data);
  writeString(Info.SourcePath, Data);
  writeBool(Info.IsStdCXXModuleInterface, Data);
}

static bool readP1689ModuleInfo(P1689ModuleInfo &Info, StringRef &Data) {
  PROPAGATE_FALSE(readString(Info.ModuleName, Data));
  PROPAGATE_FALSE(readString(Info.SourcePath, Data));
  PROPAGATE_FALSE(readBool(Info.IsStdCXXModuleInterface, Data));
  return true;
}

static void writeProvidedAndRequiredStdCXXModules(
    const std::optional<P1689ModuleInfo> &Provided,
    const std::vector<P1689ModuleInfo> &Requires, SmallString<128> &Data) {
  writeBool(Provided.has_value(), Data);
  if (Provided.has_value())
    writeP1689ModuleInfo(*Provided, Data);
  for (const P1689ModuleInfo &Req : Requires)
    writeP1689ModuleInfo(Req, Data);
  writeNull(Data);
}

static bool
parseAndHandleProvidedAndRequiredStdCXXModules(DependencyConsumer &Underlying,
                                               StringRef &Data) {
  bool HasProvided;
  std::optional<P1689ModuleInfo> Provided;
  std::vector<P1689ModuleInfo> Requires;

  PROPAGATE_FALSE(readBool(HasProvided, Data));
  if (HasProvided)
    PROPAGATE_FALSE(readP1689ModuleInfo(Provided.emplace(), Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readP1689ModuleInfo(Requires.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));

  Underlying.handleProvidedAndRequiredStdCXXModules(std::move(Provided),
                                                    std::move(Requires));
  return true;
}

static void writeBuildCommand(const Command &Cmd, SmallString<128> &Data) {
  writeString(Cmd.Executable, Data);
  writeBool(Cmd.TUCacheKey.has_value(), Data);
  if (Cmd.TUCacheKey.has_value())
    writeString(*Cmd.TUCacheKey, Data);
  for (StringRef Arg : Cmd.Arguments)
    writeString(Arg, Data);
  writeNull(Data);
}

static bool parseAndHandleBuildCommand(DependencyConsumer &Underlying,
                                       StringRef &Data) {
  Command Cmd;
  bool HasTUCacheKey;

  PROPAGATE_FALSE(readString(Cmd.Executable, Data));
  PROPAGATE_FALSE(readBool(HasTUCacheKey, Data));
  if (HasTUCacheKey)
    PROPAGATE_FALSE(readString(Cmd.TUCacheKey.emplace(), Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readString(Cmd.Arguments.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));

  Underlying.handleBuildCommand(std::move(Cmd));
  return true;
}

static bool parseAndHandleDependencyOutputOpts(DependencyConsumer &Underlying,
                                               StringRef &Data) {
  DependencyOutputOptions Opts;
  // TODO: Parse from \c Data.
  Underlying.handleDependencyOutputOpts(Opts);
  return true;
}

static void writeFileDependency(StringRef Filename, SmallString<128> &Data) {
  writeString(Filename, Data);
}

static bool parseAndHandleFileDependency(DependencyConsumer &Underlying,
                                         StringRef &Data) {
  std::string Filename;

  PROPAGATE_FALSE(readString(Filename, Data));

  Underlying.handleFileDependency(Filename);
  return true;
}

static void writePrebuiltModuleDep(const PrebuiltModuleDep &PMD,
                                   SmallString<128> &Data) {
  writeString(PMD.ModuleName, Data);
  writeString(PMD.PCMFile, Data);
  writeString(PMD.ModuleMapFile, Data);
  writeBool(PMD.ModuleCacheKey.has_value(), Data);
  if (PMD.ModuleCacheKey.has_value())
    writeString(*PMD.ModuleCacheKey, Data);
}

static bool readPrebuiltModuleDep(PrebuiltModuleDep &PMD, StringRef &Data) {
  bool HasModuleCacheKey;

  PROPAGATE_FALSE(readString(PMD.ModuleName, Data));
  PROPAGATE_FALSE(readString(PMD.PCMFile, Data));
  PROPAGATE_FALSE(readString(PMD.ModuleMapFile, Data));
  PROPAGATE_FALSE(readBool(HasModuleCacheKey, Data));
  if (HasModuleCacheKey)
    PROPAGATE_FALSE(readString(PMD.ModuleCacheKey.emplace(), Data));

  return true;
}

static bool
parseAndHandlePrebuiltModuleDependency(DependencyConsumer &Underlying,
                                       StringRef &Data) {
  PrebuiltModuleDep PMD;

  PROPAGATE_FALSE(readPrebuiltModuleDep(PMD, Data));

  Underlying.handlePrebuiltModuleDependency(std::move(PMD));
  return true;
}

static void writeModuleID(const ModuleID &ID, SmallString<128> &Data) {
  writeString(ID.ModuleName, Data);
  writeString(ID.ContextHash, Data);
}

static bool readModuleID(ModuleID &ID, StringRef &Data) {
  PROPAGATE_FALSE(readString(ID.ModuleName, Data));
  PROPAGATE_FALSE(readString(ID.ContextHash, Data));
  return true;
}

static void writeModuleDependency(const ModuleDeps &MD,
                                  SmallString<128> &Data) {
  writeModuleID(MD.ID, Data);
  writeBool(MD.IsSystem, Data);
  writeBool(MD.IgnoreCWD, Data);
  writeBool(MD.IsInStableDirectories, Data);
  writeString(MD.ClangModuleMapFile, Data);
  for (StringRef ModMap : MD.ModuleMapFileDeps)
    writeString(ModMap, Data);
  writeNull(Data);
  for (const PrebuiltModuleDep &PMD : MD.PrebuiltModuleDeps)
    writePrebuiltModuleDep(PMD, Data);
  writeNull(Data);
  for (const ModuleID &Dep : MD.ClangModuleDeps)
    writeModuleID(Dep, Data);
  writeNull(Data);
  writeBool(MD.IncludeTreeID.has_value(), Data);
  if (MD.IncludeTreeID.has_value())
    writeString(*MD.IncludeTreeID, Data);
  writeBool(MD.ModuleCacheKey.has_value(), Data);
  if (MD.ModuleCacheKey.has_value())
    writeString(*MD.ModuleCacheKey, Data);
  for (const Module::LinkLibrary &Link : MD.LinkLibraries) {
    writeString(Link.Library, Data);
    writeBool(Link.IsFramework, Data);
  }
  writeNull(Data);
  MD.forEachFileDep([&](StringRef File) { writeString(File, Data); });
  writeNull(Data);
  for (StringRef Arg : MD.getBuildArguments())
    writeString(Arg, Data);
  writeNull(Data);
}

static bool parseAndHandleModuleDependency(DependencyConsumer &Underlying,
                                           StringRef &Data) {
  ModuleDeps MD;
  bool HasIncludeTreeID;
  bool HasModuleCacheKey;
  // TODO: Propagate this to MD.
  std::vector<std::string> FileDeps;
  // TODO: Propagate this to MD.
  std::vector<std::string> BuildArguments;

  PROPAGATE_FALSE(readModuleID(MD.ID, Data));
  PROPAGATE_FALSE(readBool(MD.IsSystem, Data));
  PROPAGATE_FALSE(readBool(MD.IgnoreCWD, Data));
  PROPAGATE_FALSE(readBool(MD.IsInStableDirectories, Data));
  PROPAGATE_FALSE(readString(MD.ClangModuleMapFile, Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readString(MD.ModuleMapFileDeps.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(
        readPrebuiltModuleDep(MD.PrebuiltModuleDeps.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readModuleID(MD.ClangModuleDeps.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));
  PROPAGATE_FALSE(readBool(HasIncludeTreeID, Data));
  if (HasIncludeTreeID)
    PROPAGATE_FALSE(readString(MD.IncludeTreeID.emplace(), Data));
  PROPAGATE_FALSE(readBool(HasModuleCacheKey, Data));
  if (HasModuleCacheKey)
    PROPAGATE_FALSE(readString(MD.ModuleCacheKey.emplace(), Data));
  while (!Data.empty() && Data[0] != '\0') {
    Module::LinkLibrary &Link = MD.LinkLibraries.emplace_back();
    PROPAGATE_FALSE(readString(Link.Library, Data));
    PROPAGATE_FALSE(readBool(Link.IsFramework, Data));
  }
  PROPAGATE_FALSE(readNull(Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readString(FileDeps.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));
  while (!Data.empty() && Data[0] != '\0')
    PROPAGATE_FALSE(readString(BuildArguments.emplace_back(), Data));
  PROPAGATE_FALSE(readNull(Data));

  Underlying.handleModuleDependency(std::move(MD));
  return true;
}

static void writeDirectModuleDependency(const ModuleID &MID,
                                        SmallString<128> &Data) {
  writeModuleID(MID, Data);
}

static bool parseAndHandleDirectModuleDependency(DependencyConsumer &Underlying,
                                                 StringRef &Data) {
  ModuleID MD;

  PROPAGATE_FALSE(readModuleID(MD, Data));

  Underlying.handleDirectModuleDependency(std::move(MD));
  return true;
}

static void writeVisibleModule(StringRef ModuleName, SmallString<128> &Data) {
  writeString(ModuleName, Data);
}

static bool parseAndHandleVisibleModule(DependencyConsumer &Underlying,
                                        StringRef &Data) {
  std::string ModuleName;

  PROPAGATE_FALSE(readString(ModuleName, Data));

  Underlying.handleVisibleModule(std::move(ModuleName));
  return true;
}

static void writeContextHash(StringRef Hash, SmallString<128> &Data) {
  writeString(Hash, Data);
}

static bool parseAndHandleContextHash(DependencyConsumer &Underlying,
                                      StringRef &Data) {
  std::string Hash;

  PROPAGATE_FALSE(readString(Hash, Data));

  Underlying.handleContextHash(std::move(Hash));
  return true;
}

static void writeIncludeTreeID(StringRef ID, SmallString<128> &Data) {
  writeString(ID, Data);
}

static bool parseAndHandleIncludeTreeID(DependencyConsumer &Underlying,
                                        StringRef &Data) {
  std::string ID;

  PROPAGATE_FALSE(readString(ID, Data));

  Underlying.handleIncludeTreeID(std::move(ID));
  return true;
}

static bool returnFalse(DependencyConsumer &Underlying, StringRef &Data) {
  llvm::dbgs() << "could not decode item name\n";
  return false;
}

namespace {
class IncludeTreeBuilder;

class CASStoringConsumer : public DependencyConsumer {
  cas::ObjectStore &DB;
  DependencyConsumer &Underlying;
  std::vector<cas::ObjectRef> Refs;

public:
  CASStoringConsumer(cas::ObjectStore &DB, DependencyConsumer &Underlying)
      : DB(DB), Underlying(Underlying) {}

  Expected<cas::ObjectRef> getResult() {
    auto Ref = DB.store(Refs, "");
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    }
    return *Ref;
  }

  void handleProvidedAndRequiredStdCXXModules(
      std::optional<P1689ModuleInfo> Provided,
      std::vector<P1689ModuleInfo> Requires) override {
    SmallString<128> Data;
    writeString("P1689", Data);
    writeProvidedAndRequiredStdCXXModules(Provided, Requires, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleProvidedAndRequiredStdCXXModules(std::move(Provided),
                                                      std::move(Requires));
  }

  void handleBuildCommand(Command Cmd) override {
    SmallString<128> Data;
    writeString("Build", Data);
    writeBuildCommand(Cmd, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleBuildCommand(std::move(Cmd));
  }

  void
  handleDependencyOutputOpts(const DependencyOutputOptions &Opts) override {
    // TODO: Store into \c DB.
    Underlying.handleDependencyOutputOpts(Opts);
  }

  void handleFileDependency(StringRef Filename) override {
    SmallString<128> Data;
    writeString("File", Data);
    writeFileDependency(Filename, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleFileDependency(Filename);
  }

  void handlePrebuiltModuleDependency(PrebuiltModuleDep PMD) override {
    SmallString<128> Data;
    writeString("Prebuilt", Data);
    writePrebuiltModuleDep(PMD, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handlePrebuiltModuleDependency(std::move(PMD));
  }

  void handleModuleDependency(ModuleDeps MD) override {
    SmallString<128> Data;
    writeString("ModDep", Data);
    writeModuleDependency(MD, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleModuleDependency(std::move(MD));
  }

  void handleDirectModuleDependency(ModuleID MD) override {
    SmallString<128> Data;
    writeString("ModID", Data);
    writeDirectModuleDependency(MD, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleDirectModuleDependency(std::move(MD));
  }

  void handleVisibleModule(std::string ModuleName) override {
    SmallString<128> Data;
    writeString("VisMod", Data);
    writeVisibleModule(ModuleName, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleVisibleModule(std::move(ModuleName));
  }

  void handleContextHash(std::string Hash) override {
    SmallString<128> Data;
    writeString("Hash", Data);
    writeContextHash(Hash, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleContextHash(std::move(Hash));
  }

  void handleIncludeTreeID(std::string ID) override {
    SmallString<128> Data;
    writeString("IncTree", Data);
    writeIncludeTreeID(ID, Data);

    auto Ref = DB.store({}, Data);
    if (Error Err = Ref.takeError()) {
      // TODO: Report up.
    } else {
      Refs.push_back(*Ref);
    }
    Underlying.handleIncludeTreeID(std::move(ID));
  }
};

class IncludeTreeActionController : public CallbackActionController {
public:
  IncludeTreeActionController(cas::ObjectStore &DB, cas::ActionCache &Cache,
                              LookupModuleOutputCallback LookupOutput)
      : CallbackActionController(LookupOutput), DB(DB), Cache(Cache) {}

  Expected<cas::IncludeTreeRoot> getIncludeTree();

private:
  bool tryReplayResult(const CompilerInvocation &ScanInvocation,
                       DependencyConsumer *&Consumer,
                       DiagnosticConsumer &DiagsConsumer) override;
  bool trySaveResult() override;

  Error initialize(CompilerInstance &ScanInstance,
                   CompilerInvocation &NewInvocation) override;
  Error finalize(CompilerInstance &ScanInstance,
                 CompilerInvocation &NewInvocation) override;
  std::optional<std::string>
  getCacheKey(const CompilerInvocation &NewInvocation) override;

  Error initializeModuleBuild(CompilerInstance &ModuleScanInstance) override;
  Error finalizeModuleBuild(CompilerInstance &ModuleScanInstance) override;
  Error finalizeModuleInvocation(CowCompilerInvocation &CI,
                                 const ModuleDeps &MD) override;

private:
  IncludeTreeBuilder &current() {
    assert(!BuilderStack.empty());
    return *BuilderStack.back();
  }

private:
  cas::ObjectStore &DB;
  cas::ActionCache &Cache;
  CASOptions CASOpts;
  llvm::PrefixMapper PrefixMapper;
  // IncludeTreePPCallbacks keeps a pointer to the current builder, so use a
  // pointer so the builder cannot move when resizing.
  SmallVector<std::unique_ptr<IncludeTreeBuilder>> BuilderStack;
  std::optional<cas::IncludeTreeRoot> IncludeTreeResult;
  llvm::StringMap<std::string> OutputToCacheKey;
  std::optional<llvm::cas::ObjectProxy> Input;
  std::unique_ptr<CASStoringConsumer> CapturingConsumer;
};

/// Callbacks for building an include-tree for a given translation unit or
/// module. The \c IncludeTreeActionController is responsiblee for pushing and
/// popping builders from the stack as modules are required.
class IncludeTreeBuilder {
public:
  IncludeTreeBuilder(cas::ObjectStore &DB, llvm::PrefixMapper &PrefixMapper)
      : DB(DB), PrefixMapper(PrefixMapper) {}

  Expected<cas::IncludeTreeRoot>
  finishIncludeTree(CompilerInstance &ScanInstance,
                    CompilerInvocation &NewInvocation);

  void enteredInclude(Preprocessor &PP, FileID FID);

  void exitedInclude(Preprocessor &PP, FileID IncludedBy, FileID Include,
                     SourceLocation ExitLoc);

  void handleHasIncludeCheck(Preprocessor &PP, bool Result);

  void moduleImport(Preprocessor &PP, const Module *M, SourceLocation EndLoc);

  void enteredSubmodule(Preprocessor &PP, Module *M, SourceLocation ImportLoc,
                        bool ForPragma);
  void exitedSubmodule(Preprocessor &PP, Module *M, SourceLocation ImportLoc,
                       bool ForPragma);

private:
  struct FilePPState {
    SrcMgr::CharacteristicKind FileCharacteristic;
    cas::ObjectRef File;
    SmallVector<cas::IncludeTree::IncludeInfo, 6> Includes;
    std::optional<cas::ObjectRef> SubmoduleName;
    llvm::SmallBitVector HasIncludeChecks;
  };

  Error addModuleInputs(ASTReader &Reader);
  Expected<cas::ObjectRef> getObjectForFile(Preprocessor &PP, FileID FID);
  Expected<cas::ObjectRef>
  getObjectForFileNonCached(FileManager &FM, const SrcMgr::FileInfo &FI);
  Expected<cas::ObjectRef> getObjectForBuffer(const SrcMgr::FileInfo &FI);
  Expected<cas::ObjectRef> addToFileList(FileManager &FM, FileEntryRef FE);
  Expected<cas::IncludeTree> getCASTreeForFileIncludes(FilePPState &&PPState);
  Expected<cas::IncludeTree::File> createIncludeFile(StringRef Filename,
                                                     cas::ObjectRef Contents);

  bool hasErrorOccurred() const { return ErrorToReport.has_value(); }

  template <typename T> std::optional<T> check(Expected<T> &&E) {
    if (!E) {
      ErrorToReport = E.takeError();
      return std::nullopt;
    }
    return *E;
  }

private:
  cas::ObjectStore &DB;
  llvm::PrefixMapper &PrefixMapper;

  std::optional<cas::ObjectRef> PCHRef;
  bool StartedEnteringIncludes = false;
  // When a PCH is used this lists the filenames of the included files as they
  // are recorded in the PCH, ordered by \p FileEntry::UID index.
  SmallVector<StringRef> PreIncludedFileNames;
  llvm::BitVector SeenIncludeFiles;
  SmallVector<cas::IncludeTree::FileList::FileEntry> IncludedFiles;
  SmallVector<cas::ObjectRef> IncludedFileLists;
  std::optional<cas::ObjectRef> PredefinesBufferRef;
  std::optional<cas::ObjectRef> ModuleIncludesBufferRef;
  std::optional<cas::ObjectRef> ModuleMapRef;
  std::optional<cas::ObjectRef> APINotesRef;
  /// When the builder is created from an existing tree, the main include tree.
  std::optional<cas::ObjectRef> MainIncludeTreeRef;
  SmallVector<FilePPState> IncludeStack;
  llvm::DenseMap<const FileEntry *, std::optional<cas::ObjectRef>>
      ObjectForFile;
  std::optional<llvm::Error> ErrorToReport;
};

/// A utility for adding \c PPCallbacks and/or \cASTReaderListener to a compiler
/// instance at the appropriate time.
struct AttachOnlyDependencyCollector : public DependencyCollector {
  using MakePPCB =
      llvm::unique_function<std::unique_ptr<PPCallbacks>(Preprocessor &)>;
  using MakeASTReaderL =
      llvm::unique_function<std::unique_ptr<ASTReaderListener>(ASTReader &R)>;
  MakePPCB CreatePPCB;
  MakeASTReaderL CreateASTReaderL;
  AttachOnlyDependencyCollector(MakePPCB CreatePPCB, MakeASTReaderL CreateL)
      : CreatePPCB(std::move(CreatePPCB)),
        CreateASTReaderL(std::move(CreateL)) {}

  void attachToPreprocessor(Preprocessor &PP) final {
    if (CreatePPCB) {
      std::unique_ptr<PPCallbacks> CB = CreatePPCB(PP);
      assert(CB);
      PP.addPPCallbacks(std::move(CB));
    }
  }

  void attachToASTReader(ASTReader &R) final {
    if (CreateASTReaderL) {
      std::unique_ptr<ASTReaderListener> L = CreateASTReaderL(R);
      assert(L);
      R.addListener(std::move(L));
    }
  }
};

struct IncludeTreePPCallbacks : public PPCallbacks {
  IncludeTreeBuilder &Builder;
  Preprocessor &PP;

public:
  IncludeTreePPCallbacks(IncludeTreeBuilder &Builder, Preprocessor &PP)
      : Builder(Builder), PP(PP) {}

  void LexedFileChanged(FileID FID, LexedFileChangeReason Reason,
                        SrcMgr::CharacteristicKind FileType, FileID PrevFID,
                        SourceLocation Loc) override {
    switch (Reason) {
    case LexedFileChangeReason::EnterFile:
      Builder.enteredInclude(PP, FID);
      break;
    case LexedFileChangeReason::ExitFile: {
      Builder.exitedInclude(PP, FID, PrevFID, Loc);
      break;
    }
    }
  }

  void HasInclude(SourceLocation Loc, StringRef FileName, bool IsAngled,
                  OptionalFileEntryRef File,
                  SrcMgr::CharacteristicKind FileType) override {
    Builder.handleHasIncludeCheck(PP, File.has_value());
  }

  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath, const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override {
    // File includes are handled by LexedFileChanged.
    if (!ModuleImported)
      return;

    // Calculate EndLoc for the directive
    // FIXME: pass EndLoc through PPCallbacks; it is already calculated
    SourceManager &SM = PP.getSourceManager();
    std::pair<FileID, unsigned> LocInfo = SM.getDecomposedExpansionLoc(HashLoc);
    StringRef Buffer = SM.getBufferData(LocInfo.first);
    Lexer L(SM.getLocForStartOfFile(LocInfo.first), PP.getLangOpts(),
            Buffer.begin(), Buffer.begin() + LocInfo.second, Buffer.end());
    L.setParsingPreprocessorDirective(true);
    Token Tok;
    do {
      L.LexFromRawLexer(Tok);
    } while (!Tok.isOneOf(tok::eod, tok::eof));
    SourceLocation EndLoc = L.getSourceLocation();

    Builder.moduleImport(PP, SuggestedModule, EndLoc);
  }

  void EnteredSubmodule(Module *M, SourceLocation ImportLoc,
                        bool ForPragma) override {
    Builder.enteredSubmodule(PP, M, ImportLoc, ForPragma);
  }
  void LeftSubmodule(Module *M, SourceLocation ImportLoc,
                     bool ForPragma) override {
    Builder.exitedSubmodule(PP, M, ImportLoc, ForPragma);
  }
};

/// Utility to trigger module lookup in header search for modules loaded via
/// PCH. This causes dependency scanning via PCH to parse modulemap files at
/// roughly the same point they would with modulemap files embedded in the pcms,
/// which is disabled with include-tree modules. Without this, we can fail to
/// find modules that are in the same directory as a named import, since
/// it may be skipped during search (see \c loadFrameworkModule).
///
/// The specific lookup we do matches what happens in ASTReader for the
/// MODULE_DIRECTORY record, and ignores the result.
class LookupPCHModulesListener : public ASTReaderListener {
public:
  LookupPCHModulesListener(ASTReader &R) : Reader(R) {}

private:
  void visitModuleFile(StringRef Filename,
                       serialization::ModuleKind Kind) final {
    // Any prebuilt or explicit modules seen during scanning are "full" modules
    // rather than implicitly built scanner modules.
    if (Kind == serialization::MK_PrebuiltModule ||
        Kind == serialization::MK_ExplicitModule) {
      serialization::ModuleManager &Manager = Reader.getModuleManager();
      serialization::ModuleFile *MF = Manager.lookupByFileName(Filename);
      assert(MF && "module file missing in visitModuleFile");
      // Match MODULE_DIRECTORY: allow full search and ignore failure to find
      // the module.
      HeaderSearch &HS = Reader.getPreprocessor().getHeaderSearchInfo();
      (void)HS.lookupModule(MF->ModuleName, SourceLocation(),
                            /*AllowSearch=*/true,
                            /*AllowExtraModuleMapSearch=*/true);
    }
  }

private:
  ASTReader &Reader;
};
} // namespace

/// The PCH recorded file paths with canonical paths, create a VFS that
/// allows remapping back to the non-canonical source paths so that they are
/// found during dep-scanning.
void dependencies::addReversePrefixMappingFileSystem(
    const llvm::PrefixMapper &PrefixMapper, CompilerInstance &ScanInstance) {
  llvm::PrefixMapper ReverseMapper;
  ReverseMapper.addInverseRange(PrefixMapper.getMappings());
  ReverseMapper.sort();
  IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS =
      llvm::vfs::createPrefixMappingFileSystem(
          std::move(ReverseMapper), &ScanInstance.getVirtualFileSystem());

  ScanInstance.setVirtualFileSystem(FS);
  ScanInstance.getFileManager().setVirtualFileSystem(std::move(FS));
}

Expected<cas::IncludeTreeRoot> IncludeTreeActionController::getIncludeTree() {
  if (IncludeTreeResult)
    return *IncludeTreeResult;
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "failed to produce include-tree");
}

bool IncludeTreeActionController::tryReplayResult(
    const CompilerInvocation &ScanInvocation, DependencyConsumer *&Consumer,
    DiagnosticConsumer &DiagsConsumer) {
  // FIXME: This unfortunately bypasses scanner's VFS with the in-memory cache.
  //
  // Let's create a VFS that uses scanner's VFS to open files, passes them into
  // the VFS that constructs the CAS FS tree, and then returns them. This means
  // that we access each file at most once (thanks to scanner's VFS), we build
  // the CAS FS tree that allows us to skip scans, and the real files reach the
  // compiler, meaning include-once, hardlinks and case-insensitivity work as
  // expected and we can construct include-tree correctly.
  IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> BaseFS;
  if (Error Err =
          llvm::cas::createCachingOnDiskFileSystem(DB).moveInto(BaseFS)) {
    llvm::handleAllErrors(std::move(Err), [](const llvm::ErrorInfoBase &) {});
    llvm::dbgs() << "could not create CachingOnDiskFileSystem\n";
    return false;
  }

  DiagnosticOptions DiagOpts;
  DiagOpts.Remarks.push_back("compile-job-cache-miss");
  DiagOpts.Remarks.push_back("compile-job-cache-hit");
  IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS;
  {
    DiagnosticsEngine Diags(DiagnosticIDs::create(), DiagOpts, &DiagsConsumer,
                            false);
    VFS =
        createVFSFromCompilerInvocation(ScanInvocation, Diags, BaseFS, nullptr);
  }

  auto Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, &DiagsConsumer,
                                          /*ShouldOwnClient=*/false);

  std::function<bool(StringRef)> Visit = [&](StringRef Path) {
    if (Path.contains("Xcode.app") && !Path.contains("0~"))
      return true;
    auto Stat = VFS->status(Path);
    if (!Stat)
      return false;
    if (!Stat->isDirectory())
      return false;
    std::error_code EC;
    for (llvm::vfs::directory_iterator It = VFS->dir_begin(Path, EC), End;
         It != End && !EC; It.increment(EC))
      if (Visit(It->path()))
        return true;
    return false;
  };

  for (const auto &SearchPath :
       ScanInvocation.getHeaderSearchOpts().UserEntries) {
    StringRef SearchPathPath = SearchPath.Path;
    std::string WithSysRoot;
    if (!SearchPath.IgnoreSysRoot) {
      WithSysRoot =
          ScanInvocation.getHeaderSearchOpts().Sysroot + SearchPath.Path;
      SearchPathPath = WithSysRoot;
    }
    if (Visit(SearchPathPath)) {
      /*
      llvm::dbgs() << "cannot cache invocation '" << SearchPathPath << "'\n";
      */
      return false;
    }
  }

  for (const auto &InvocationInput : ScanInvocation.getFrontendOpts().Inputs)
    if (InvocationInput.isFile())
      if (Visit(llvm::sys::path::parent_path(InvocationInput.getFile()))) {
        /*
        llvm::dbgs() << "cannot cache invocation '" << InvocationInput.getFile()
                     << "'\n";
        */
        return false;
      }

  if (Error Err = BaseFS->createTreeFromAllAccesses().moveInto(Input)) {
    llvm::handleAllErrors(std::move(Err), [](const llvm::ErrorInfoBase &) {});
    llvm::dbgs() << "failed to createTreeFromAllAccesses\n";
    return false;
  }

  std::optional<cas::CASID> ResultID;
  if (Error Err = Cache.get(*Input).moveInto(ResultID)) {
    llvm::handleAllErrors(std::move(Err), [](const llvm::ErrorInfoBase &) {});
    llvm::dbgs() << "failed to query the action cache\n";
    return false;
  }

  if (ResultID) {
    llvm::dbgs() << "cache hit for '" << Input->getID().toString() << "' -> '"
                 << ResultID->toString() << "'\n";
    Diags->Report(diag::remark_scan_job_cache_hit)
        << Input->getID().toString() << ResultID->toString();

    std::optional<cas::ObjectProxy> Proxy;
    if (Error Err = DB.getProxy(*ResultID).moveInto(Proxy)) {
      llvm::handleAllErrors(std::move(Err), [](const llvm::ErrorInfoBase &) {});
      llvm::dbgs() << "failed to get proxy of the cached result\n";
      return false;
    }

    for (unsigned I = 0, E = Proxy->getNumReferences(); I != E; ++I) {
      cas::ObjectRef Item = Proxy->getReference(I);
      std::optional<cas::ObjectProxy> Proxy;
      if (Error Err = DB.getProxy(Item).moveInto(Proxy)) {
        llvm::handleAllErrors(std::move(Err),
                              [](const llvm::ErrorInfoBase &) {});
        llvm::dbgs() << "failed to get proxy of the cached result sub-object\n";
        return false;
      }

      StringRef Data = Proxy->getData();
      StringRef Kind, Content;
      std::tie(Kind, Content) = Data.split('\0');
      if (Content.empty()) {
        llvm::dbgs() << "failed to extract the content of an item\n";
        return false;
      }

      auto HandleItem =
          llvm::StringSwitch<bool (*)(DependencyConsumer &, StringRef &)>(Kind)
              .Case("P1689", &parseAndHandleProvidedAndRequiredStdCXXModules)
              .Case("Build", &parseAndHandleBuildCommand)
              .Case("<TBD>", &parseAndHandleDependencyOutputOpts)
              .Case("File", &parseAndHandleFileDependency)
              .Case("Prebuilt", &parseAndHandlePrebuiltModuleDependency)
              .Case("ModDep", &parseAndHandleModuleDependency)
              .Case("ModID", &parseAndHandleDirectModuleDependency)
              .Case("VisMod", &parseAndHandleVisibleModule)
              .Case("Hash", &parseAndHandleContextHash)
              .Case("IncTree", &parseAndHandleIncludeTreeID)
              .Default(&returnFalse);

      if (!HandleItem(*Consumer, Data))
        return false;
    }

    return true;
  }

  llvm::dbgs() << "cache miss for '" << Input->getID().toString() << "'\n";
  Diags->Report(diag::remark_scan_job_cache_miss) << Input->getID().toString();

  // Interpose our custom consumer that will store incoming information into CAS
  // (for replay) and forward then to the original consumer.
  CapturingConsumer = std::make_unique<CASStoringConsumer>(DB, *Consumer);
  Consumer = CapturingConsumer.get();

  return false;
}

Error IncludeTreeActionController::initialize(
    CompilerInstance &ScanInstance, CompilerInvocation &NewInvocation) {
  DepscanPrefixMapping::configurePrefixMapper(NewInvocation, PrefixMapper);

  auto ensurePathRemapping = [&]() {
    if (PrefixMapper.empty())
      return;

    PreprocessorOptions &PPOpts = ScanInstance.getPreprocessorOpts();
    if (PPOpts.Includes.empty() && PPOpts.ImplicitPCHInclude.empty() &&
        !ScanInstance.getLangOpts().Modules)
      return;

    addReversePrefixMappingFileSystem(PrefixMapper, ScanInstance);

    // TODO: Confirm why it's not enough to do this in
    // DepscanPrefixMapping::remapInvocationPaths.
    // These are written in the predefines buffer, so we need to remap them.
    for (std::string &Include : PPOpts.Includes)
      PrefixMapper.mapInPlace(Include);
  };
  ensurePathRemapping();

  BuilderStack.push_back(
      std::make_unique<IncludeTreeBuilder>(DB, PrefixMapper));

  // Attach callbacks for the IncludeTree of the TU. The preprocessor
  // does not exist yet, so we need to indirect this via DependencyCollector.
  auto DC = std::make_shared<AttachOnlyDependencyCollector>(
      [&Builder = current()](Preprocessor &PP) {
        return std::make_unique<IncludeTreePPCallbacks>(Builder, PP);
      },
      [](ASTReader &R) {
        return std::make_unique<LookupPCHModulesListener>(R);
      });
  ScanInstance.addDependencyCollector(std::move(DC));

  // Enable caching in the resulting commands.
  ScanInstance.getFrontendOpts().CacheCompileJob = true;
  ScanInstance.getFrontendOpts().ForIncludeTreeScan = true;
  CASOpts = ScanInstance.getCASOpts();

  return Error::success();
}

Error IncludeTreeActionController::finalize(CompilerInstance &ScanInstance,
                                            CompilerInvocation &NewInvocation) {
  auto GetInputCacheKey = [&]() -> std::optional<StringRef> {
    if (NewInvocation.getFrontendOpts().Inputs.size() != 1)
      return {};
    const auto &FIF = NewInvocation.getFrontendOpts().Inputs.front();
    if (!FIF.isFile())
      return {};
    auto It = OutputToCacheKey.find(FIF.getFile());
    if (It == OutputToCacheKey.end())
      return {};

    return It->second;
  };

  std::string InputID;
  CachingInputKind InputKind;
  if (auto InputCacheKey = GetInputCacheKey()) {
    InputID = InputCacheKey->str();
    InputKind = CachingInputKind::CachedCompilation;
  } else {
    assert(!IncludeTreeResult);
    assert(BuilderStack.size() == 1);
    auto Builder = BuilderStack.pop_back_val();
    Error E = Builder->finishIncludeTree(ScanInstance, NewInvocation)
                  .moveInto(IncludeTreeResult);
    if (E)
      return E;
    InputID = IncludeTreeResult->getID().toString();
    InputKind = CachingInputKind::IncludeTree;
  }

  configureInvocationForCaching(NewInvocation, CASOpts, InputID, InputKind,
                                // FIXME: working dir?
                                /*CASFSWorkingDir=*/"");

  DepscanPrefixMapping::remapInvocationPaths(NewInvocation, PrefixMapper);

  auto &CAS = ScanInstance.getOrCreateObjectStore();
  // FIXME: Make this return an error and propagate it up.
  auto Key = createCompileJobCacheKey(CAS, ScanInstance.getDiagnostics(),
                                      NewInvocation);
  if (Key)
    OutputToCacheKey[NewInvocation.getFrontendOpts().OutputFile] =
        Key->toString();
  return Error::success();
}

bool IncludeTreeActionController::trySaveResult() {
  if (CapturingConsumer) {
    auto Data = CapturingConsumer->getResult();
    if (Error Err = Data.takeError()) {
      llvm::handleAllErrors(std::move(Err), [](llvm::ErrorInfoBase &) {});
      return false;
    }

    assert(Input);

    if (Error Err = Cache.put(*Input, DB.getID(*Data))) {
      llvm::handleAllErrors(std::move(Err), [](llvm::ErrorInfoBase &) {});
      llvm::dbgs() << "failed to store the entry in action cache\n";
      return false;
    }

    llvm::dbgs() << "Scanning cache store for key " << Input->getID().toString()
                 << " with value " << DB.getID(*Data).toString() << "\n";

    return true;
  }

  return true;
}

std::optional<std::string> IncludeTreeActionController::getCacheKey(
    const CompilerInvocation &NewInvocation) {
  auto It = OutputToCacheKey.find(NewInvocation.getFrontendOpts().OutputFile);
  // FIXME: Assert this does not happen.
  if (It == OutputToCacheKey.end())
    return std::nullopt;
  return It->second;
}

Error IncludeTreeActionController::initializeModuleBuild(
    CompilerInstance &ModuleScanInstance) {
  BuilderStack.push_back(
      std::make_unique<IncludeTreeBuilder>(DB, PrefixMapper));

  // Attach callbacks for the IncludeTree of the module. The preprocessor
  // does not exist yet, so we need to indirect this via DependencyCollector.
  auto DC = std::make_shared<AttachOnlyDependencyCollector>(
      [&Builder = current()](Preprocessor &PP) {
        return std::make_unique<IncludeTreePPCallbacks>(Builder, PP);
      },
      [](ASTReader &R) {
        return std::make_unique<LookupPCHModulesListener>(R);
      });
  ModuleScanInstance.addDependencyCollector(std::move(DC));
  ModuleScanInstance.setPrefixMapper(PrefixMapper);

  return Error::success();
}

Error IncludeTreeActionController::finalizeModuleBuild(
    CompilerInstance &ModuleScanInstance) {
  // FIXME: the scan invocation is incorrect here; we need the `NewInvocation`
  // from `finalizeModuleInvocation` to finish the tree.
  resetBenignCodeGenOptions(
      frontend::GenerateModule,
      ModuleScanInstance.getInvocation().getLangOpts(),
      ModuleScanInstance.getInvocation().getCodeGenOpts());
  auto Builder = BuilderStack.pop_back_val();

  // If there was an error, bail out early. The state of `Builder` may be
  // inconsistent since there is no guarantee that exitedInclude or
  // finalizeModuleBuild have been called for all imports.
  if (ModuleScanInstance.getDiagnostics().hasUnrecoverableErrorOccurred())
    return Error::success(); // Already reported.

  auto Tree = Builder->finishIncludeTree(ModuleScanInstance,
                                         ModuleScanInstance.getInvocation());
  if (!Tree)
    return Tree.takeError();

  ModuleScanInstance.getPreprocessor().setCASIncludeTreeID(
      Tree->getID().toString());

  return Error::success();
}

Error IncludeTreeActionController::finalizeModuleInvocation(
    CowCompilerInvocation &CowCI, const ModuleDeps &MD) {
  if (!MD.IncludeTreeID)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "missing include-tree for module '%s'",
                                   MD.ID.ModuleName.c_str());

  // TODO: Avoid this copy.
  CompilerInvocation CI(CowCI);

  configureInvocationForCaching(CI, CASOpts, *MD.IncludeTreeID,
                                CachingInputKind::IncludeTree,
                                /*CASFSWorkingDir=*/"");

  DepscanPrefixMapping::remapInvocationPaths(CI, PrefixMapper);

  CowCI = CI;
  return Error::success();
}

void IncludeTreeBuilder::enteredInclude(Preprocessor &PP, FileID FID) {
  if (hasErrorOccurred())
    return;

  if (!StartedEnteringIncludes) {
    StartedEnteringIncludes = true;

    SmallVector<OptionalFileEntryRef> UIDToFE;
    PP.getFileManager().GetUniqueIDMapping(UIDToFE);

    // Get the included files (coming from a PCH), and keep track of the
    // filenames that were recorded in the PCH.
    for (const FileEntry *FE : PP.getIncludedFiles()) {
      unsigned UID = FE->getUID();
      if (UID >= PreIncludedFileNames.size())
        PreIncludedFileNames.resize(UID + 1);
      OptionalFileEntryRef FERef = UIDToFE[FE->getUID()];
      assert(FERef && "No FileEntryRef with given UID");
      PreIncludedFileNames[UID] = FERef->getName();
    }
  }

  std::optional<cas::ObjectRef> FileRef = check(getObjectForFile(PP, FID));
  if (!FileRef)
    return;
  const SrcMgr::FileInfo &FI =
      PP.getSourceManager().getSLocEntry(FID).getFile();
  IncludeStack.push_back({FI.getFileCharacteristic(), *FileRef, {}, {}, {}});
}

void IncludeTreeBuilder::exitedInclude(Preprocessor &PP, FileID IncludedBy,
                                       FileID Include, SourceLocation ExitLoc) {
  if (hasErrorOccurred())
    return;

  assert(*check(getObjectForFile(PP, Include)) == IncludeStack.back().File);
  std::optional<cas::IncludeTree> IncludeTree =
      check(getCASTreeForFileIncludes(IncludeStack.pop_back_val()));
  if (!IncludeTree)
    return;
  assert(*check(getObjectForFile(PP, IncludedBy)) == IncludeStack.back().File);
  SourceManager &SM = PP.getSourceManager();
  std::pair<FileID, unsigned> LocInfo = SM.getDecomposedExpansionLoc(ExitLoc);

  // If the exited header belongs to a sub-module that's marked as missing from
  // the umbrella, we must've first loaded its PCM file to find that out.
  // We need to match this behavior with include-tree. Let's mark this as
  // spurious import. For this node, Clang will load the top-level module, emit
  // the appropriate diagnostics and then fall back to textual inclusion of the
  // header itself.
  if (auto FE = PP.getSourceManager().getFileEntryRefForID(Include)) {
    ModuleMap &ModMap = PP.getHeaderSearchInfo().getModuleMap();
    Module *M = ModMap.findModuleForHeader(*FE).getModule();
    if (M && M->IsInferredMissingFromUmbrellaHeader) {
      assert(!IncludeTree->isSubmodule() &&
             "Include of header missing from umbrella header is modular");

      moduleImport(PP, M, ExitLoc);
      auto Import = IncludeStack.back().Includes.pop_back_val();

      auto SpuriousImport = check(cas::IncludeTree::SpuriousImport::create(
          DB, Import.Ref, IncludeTree->getRef()));
      if (!SpuriousImport)
        return;
      IncludeStack.back().Includes.push_back(
          {SpuriousImport->getRef(), LocInfo.second,
           cas::IncludeTree::NodeKind::SpuriousImport});
      return;
    }
  }

  IncludeStack.back().Includes.push_back({IncludeTree->getRef(), LocInfo.second,
                                          cas::IncludeTree::NodeKind::Tree});
}

void IncludeTreeBuilder::handleHasIncludeCheck(Preprocessor &PP, bool Result) {
  if (hasErrorOccurred())
    return;

  IncludeStack.back().HasIncludeChecks.push_back(Result);
}

void IncludeTreeBuilder::moduleImport(Preprocessor &PP, const Module *M,
                                      SourceLocation EndLoc) {
  bool VisibilityOnly = M->isForBuilding(PP.getLangOpts());
  auto Import = check(cas::IncludeTree::ModuleImport::create(
      DB, M->getFullModuleName(), VisibilityOnly));
  if (!Import)
    return;

  std::pair<FileID, unsigned> EndLocInfo =
      PP.getSourceManager().getDecomposedExpansionLoc(EndLoc);
  IncludeStack.back().Includes.push_back(
      {Import->getRef(), EndLocInfo.second,
       cas::IncludeTree::NodeKind::ModuleImport});
}

void IncludeTreeBuilder::enteredSubmodule(Preprocessor &PP, Module *M,
                                          SourceLocation ImportLoc,
                                          bool ForPragma) {
  if (ForPragma)
    return; // Will be parsed as normal.
  if (hasErrorOccurred())
    return;
  assert(!IncludeStack.back().SubmoduleName && "repeated enteredSubmodule");
  auto Ref = check(DB.storeFromString({}, M->getFullModuleName()));
  IncludeStack.back().SubmoduleName = Ref;
}
void IncludeTreeBuilder::exitedSubmodule(Preprocessor &PP, Module *M,
                                         SourceLocation ImportLoc,
                                         bool ForPragma) {
  // Submodule exit is handled automatically when leaving a modular file.
}

static Expected<cas::IncludeTree::Module>
getIncludeTreeModule(cas::ObjectStore &DB, Module *M) {
  using ITModule = cas::IncludeTree::Module;
  SmallVector<cas::ObjectRef> Submodules;
  for (Module *Sub : M->submodules()) {
    Expected<ITModule> SubTree = getIncludeTreeModule(DB, Sub);
    if (!SubTree)
      return SubTree.takeError();
    Submodules.push_back(SubTree->getRef());
  }

  ITModule::ModuleFlags Flags;
  Flags.IsFramework = M->IsFramework;
  Flags.IsExplicit = M->IsExplicit;
  Flags.IsExternC = M->IsExternC;
  Flags.IsSystem = M->IsSystem;
  Flags.InferSubmodules = M->InferSubmodules;
  Flags.InferExplicitSubmodules = M->InferExplicitSubmodules;
  Flags.InferExportWildcard = M->InferExportWildcard;
  Flags.UseExportAsModuleLinkName = M->UseExportAsModuleLinkName;

  bool GlobalWildcardExport = false;
  SmallVector<ITModule::ExportList::Export> Exports;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  for (Module::ExportDecl &Export : M->Exports) {
    if (Export.getPointer() == nullptr && Export.getInt()) {
      GlobalWildcardExport = true;
    } else if (Export.getPointer()) {
      StringRef Name = Saver.save(Export.getPointer()->getFullModuleName());
      Exports.push_back({Name, Export.getInt()});
    }
  }
  std::optional<cas::ObjectRef> ExportList;
  if (GlobalWildcardExport || !Exports.empty()) {
    auto EL = ITModule::ExportList::create(DB, Exports, GlobalWildcardExport);
    if (!EL)
      return EL.takeError();
    ExportList = EL->getRef();
  }

  SmallVector<ITModule::LinkLibraryList::LinkLibrary> Libraries;
  for (Module::LinkLibrary &LL : M->LinkLibraries) {
    Libraries.push_back({LL.Library, LL.IsFramework});
  }
  std::optional<cas::ObjectRef> LinkLibraries;
  if (!Libraries.empty()) {
    auto LL = ITModule::LinkLibraryList::create(DB, Libraries);
    if (!LL)
      return LL.takeError();
    LinkLibraries = LL->getRef();
  }

  return ITModule::create(DB, M->Name, M->ExportAsModule, Flags, Submodules,
                          ExportList, LinkLibraries);
}

Expected<cas::IncludeTreeRoot>
IncludeTreeBuilder::finishIncludeTree(CompilerInstance &ScanInstance,
                                      CompilerInvocation &NewInvocation) {
  if (ErrorToReport)
    return std::move(*ErrorToReport);

  FileManager &FM = ScanInstance.getFileManager();

  auto addFile = [&](StringRef FilePath,
                     bool IgnoreFileError = false) -> Error {
    if (FilePath.empty())
      return Error::success();
    llvm::Expected<FileEntryRef> FE = FM.getFileRef(FilePath);
    if (!FE) {
      auto Err = FE.takeError();
      if (IgnoreFileError) {
        llvm::consumeError(std::move(Err));
        return Error::success();
      }
      return Err;
    }
    std::optional<cas::ObjectRef> Ref;
    return addToFileList(FM, *FE).moveInto(Ref);
  };

  for (StringRef FilePath : NewInvocation.getLangOpts().NoSanitizeFiles) {
    if (Error E = addFile(FilePath))
      return std::move(E);
  }
  // Add profile files.
  // FIXME: Do not have the logic here to determine which path should be set
  // but ideally only the path needed for the compilation is set and we already
  // checked the file needed exists. Just try load and ignore errors.
  if (Error E = addFile(NewInvocation.getCodeGenOpts().ProfileInstrumentUsePath,
                        /*IgnoreFileError=*/true))
    return std::move(E);
  if (Error E = addFile(NewInvocation.getCodeGenOpts().SampleProfileFile,
                        /*IgnoreFileError=*/true))
    return std::move(E);
  if (Error E = addFile(NewInvocation.getCodeGenOpts().ProfileRemappingFile,
                        /*IgnoreFileError=*/true))
    return std::move(E);

  StringRef Sysroot = NewInvocation.getHeaderSearchOpts().Sysroot;
  if (!Sysroot.empty()) {
    // Include 'SDKSettings.json', if it exists, to accomodate availability
    // checks during the compilation.
    llvm::SmallString<256> FilePath = Sysroot;
    llvm::sys::path::append(FilePath, "SDKSettings.json");
    if (Error E = addFile(FilePath, /*IgnoreFileError*/ true))
      return std::move(E);
  }

  auto FinishIncludeTree = [&]() -> Error {
    IntrusiveRefCntPtr<ASTReader> Reader = ScanInstance.getASTReader();
    if (!Reader)
      return Error::success(); // no need for additional work.

    // Go through all the recorded input files.
    if (Error E = addModuleInputs(*Reader))
      return E;

    PreprocessorOptions &PPOpts = NewInvocation.getPreprocessorOpts();
    if (PPOpts.ImplicitPCHInclude.empty())
      return Error::success(); // no need for additional work.

    llvm::ErrorOr<cas::ObjectRef> CASContents =
        FM.getObjectRefForFileContent(PPOpts.ImplicitPCHInclude);
    if (!CASContents)
      return llvm::errorCodeToError(CASContents.getError());

    auto PCHFile = cas::IncludeTree::File::create(DB, "<PCH>", *CASContents);
    if (!PCHFile)
      return PCHFile.takeError();
    PCHRef = PCHFile->getRef();
    return llvm::Error::success();
  };

  if (Error E = FinishIncludeTree())
    return std::move(E);

  if (ErrorToReport)
    return std::move(*ErrorToReport);

  assert(IncludeStack.size() == 1);
  Expected<cas::IncludeTree> MainIncludeTree =
      getCASTreeForFileIncludes(IncludeStack.pop_back_val());
  if (!MainIncludeTree)
    return MainIncludeTree.takeError();

  if (!ScanInstance.getLangOpts().CurrentModule.empty()) {
    SmallVector<cas::ObjectRef> Modules;
    SmallVector<cas::ObjectRef> APINotes;
    auto AddModule = [&](Module *M) -> llvm::Error {
      Expected<cas::IncludeTree::Module> Mod = getIncludeTreeModule(DB, M);
      if (!Mod)
        return Mod.takeError();
      Modules.push_back(Mod->getRef());
      return Error::success();
    };
    if (Module *M = ScanInstance.getPreprocessor().getCurrentModule()) {
      if (Error E = AddModule(M))
        return std::move(E);

      // If it is currently module, load its APINotes.
      api_notes::APINotesManager ANM(ScanInstance.getSourceManager(),
                                     ScanInstance.getLangOpts());
      auto Notes = ANM.getCurrentModuleAPINotes(
          M, ScanInstance.getLangOpts().APINotesModules,
          ScanInstance.getAPINotesOpts().ModuleSearchPaths);
      for (auto File : Notes) {
        if (auto Buf =
                ScanInstance.getSourceManager().getMemoryBufferForFileOrNone(
                    File)) {
          auto Note = DB.storeFromString({}, Buf->getBuffer());
          if (!Note)
            return Note.takeError();
          APINotes.push_back(*Note);
        }
      }
    } else {
      // When building a TU or PCH, we can have headers files that are part of
      // both the public and private modules that are included textually. In
      // that case we need both of those modules.
      ModuleMap &MMap =
          ScanInstance.getPreprocessor().getHeaderSearchInfo().getModuleMap();
      if (Module *M = MMap.findModule(ScanInstance.getLangOpts().CurrentModule))
        if (Error E = AddModule(M))
          return std::move(E);
      if (Module *PM = MMap.findModule(ScanInstance.getLangOpts().ModuleName +
                                       "_Private"))
        if (Error E = AddModule(PM))
          return std::move(E);
    }

    auto ModMap = cas::IncludeTree::ModuleMap::create(DB, Modules);
    if (!ModMap)
      return ModMap.takeError();
    ModuleMapRef = ModMap->getRef();

    if (!APINotes.empty()) {
      auto ModAPINotes = cas::IncludeTree::APINotes::create(DB, APINotes);
      if (!ModAPINotes)
        return ModAPINotes.takeError();
      APINotesRef = ModAPINotes->getRef();
    }
  }

  auto FileList =
      cas::IncludeTree::FileList::create(DB, IncludedFiles, IncludedFileLists);
  if (!FileList)
    return FileList.takeError();

  return cas::IncludeTreeRoot::create(DB, MainIncludeTree->getRef(),
                                      FileList->getRef(), PCHRef, ModuleMapRef,
                                      APINotesRef);
}

Error IncludeTreeBuilder::addModuleInputs(ASTReader &Reader) {
  for (serialization::ModuleFile &MF : Reader.getModuleManager()) {
    // Only add direct imports to avoid duplication. Each include tree is a
    // superset of its imported modules' include trees.
    if (!MF.isDirectlyImported())
      continue;

    assert(!MF.IncludeTreeID.empty() && "missing include-tree for import");

    std::optional<cas::CASID> ID;
    if (Error E = DB.parseID(MF.IncludeTreeID).moveInto(ID))
      return E;
    std::optional<cas::ObjectRef> Ref = DB.getReference(*ID);
    if (!Ref)
      return DB.createUnknownObjectError(*ID);
    std::optional<cas::IncludeTreeRoot> Root;
    if (Error E = cas::IncludeTreeRoot::get(DB, *Ref).moveInto(Root))
      return E;

    IncludedFileLists.push_back(Root->getFileListRef());
  }

  return Error::success();
}

Expected<cas::ObjectRef> IncludeTreeBuilder::getObjectForFile(Preprocessor &PP,
                                                              FileID FID) {
  SourceManager &SM = PP.getSourceManager();
  const SrcMgr::FileInfo &FI = SM.getSLocEntry(FID).getFile();
  if (PP.getPredefinesFileID() == FID) {
    if (!PredefinesBufferRef) {
      auto Ref = getObjectForBuffer(FI);
      if (!Ref)
        return Ref.takeError();
      PredefinesBufferRef = *Ref;
    }
    return *PredefinesBufferRef;
  }
  if (!FI.getContentCache().OrigEntry &&
      FI.getName() == Module::getModuleInputBufferName()) {
    // Virtual <module-includes> buffer
    if (!ModuleIncludesBufferRef) {
      if (Error E = getObjectForBuffer(FI).moveInto(ModuleIncludesBufferRef))
        return std::move(E);
    }
    return *ModuleIncludesBufferRef;
  }
  assert(FI.getContentCache().OrigEntry);
  auto &FileRef = ObjectForFile[*FI.getContentCache().OrigEntry];
  if (!FileRef) {
    auto Ref = getObjectForFileNonCached(SM.getFileManager(), FI);
    if (!Ref)
      return Ref.takeError();
    FileRef = *Ref;
  }
  return *FileRef;
}

Expected<cas::ObjectRef>
IncludeTreeBuilder::getObjectForFileNonCached(FileManager &FM,
                                              const SrcMgr::FileInfo &FI) {
  OptionalFileEntryRef FE = FI.getContentCache().OrigEntry;
  assert(FE);

  // Mark the include as already seen.
  if (FE->getUID() >= SeenIncludeFiles.size())
    SeenIncludeFiles.resize(FE->getUID() + 1);
  SeenIncludeFiles.set(FE->getUID());

  return addToFileList(FM, *FE);
}

Expected<cas::ObjectRef>
IncludeTreeBuilder::getObjectForBuffer(const SrcMgr::FileInfo &FI) {
  // This is a non-file buffer, like the predefines.
  auto Ref = DB.storeFromString(
      {}, FI.getContentCache().getBufferIfLoaded()->getBuffer());
  if (!Ref)
    return Ref.takeError();
  Expected<cas::IncludeTree::File> FileNode =
      createIncludeFile(FI.getName(), *Ref);
  if (!FileNode)
    return FileNode.takeError();
  return FileNode->getRef();
}

Expected<cas::ObjectRef> IncludeTreeBuilder::addToFileList(FileManager &FM,
                                                           FileEntryRef FE) {
  SmallString<128> PathStorage;
  StringRef Filename = FE.getName();
  // Apply -working-directory to relative paths. This option causes filesystem
  // lookups to use absolute paths, so make paths in the include-tree filesystem
  // absolute to match.
  if (!llvm::sys::path::is_absolute(Filename) &&
      !FM.getFileSystemOpts().WorkingDir.empty()) {
    PathStorage = Filename;
    FM.FixupRelativePath(PathStorage);
    Filename = PathStorage;
  }

  llvm::ErrorOr<cas::ObjectRef> CASContents =
      FM.getObjectRefForFileContent(Filename);
  if (!CASContents)
    return llvm::errorCodeToError(CASContents.getError());

  auto addFile = [&](StringRef Filename) -> Expected<cas::ObjectRef> {
    assert(!Filename.empty());
    auto FileNode = createIncludeFile(Filename, *CASContents);
    if (!FileNode)
      return FileNode.takeError();
    IncludedFiles.push_back(
        {FileNode->getRef(),
         static_cast<cas::IncludeTree::FileList::FileSizeTy>(FE.getSize())});
    return FileNode->getRef();
  };

  // Check whether another path coming from the PCH is associated with the same
  // file.
  unsigned UID = FE.getUID();
  if (UID < PreIncludedFileNames.size() && !PreIncludedFileNames[UID].empty() &&
      PreIncludedFileNames[UID] != Filename) {
    auto FileNode = addFile(PreIncludedFileNames[UID]);
    if (!FileNode)
      return FileNode.takeError();
  }

  return addFile(Filename);
}

Expected<cas::IncludeTree>
IncludeTreeBuilder::getCASTreeForFileIncludes(FilePPState &&PPState) {
  return cas::IncludeTree::create(DB, PPState.FileCharacteristic, PPState.File,
                                  PPState.Includes, PPState.SubmoduleName,
                                  PPState.HasIncludeChecks);
}

Expected<cas::IncludeTree::File>
IncludeTreeBuilder::createIncludeFile(StringRef Filename,
                                      cas::ObjectRef Contents) {
  SmallString<256> MappedPath;
  if (!PrefixMapper.empty()) {
    PrefixMapper.map(Filename, MappedPath);
    Filename = MappedPath;
  }
  return cas::IncludeTree::File::create(DB, Filename, std::move(Contents));
}

std::unique_ptr<DependencyActionController>
dependencies::createIncludeTreeActionController(
    LookupModuleOutputCallback LookupModuleOutput, cas::ObjectStore &DB,
    cas::ActionCache &Cache) {
  return std::make_unique<IncludeTreeActionController>(DB, Cache,
                                                       LookupModuleOutput);
}

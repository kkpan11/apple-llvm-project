//===-- InstrumentationRuntimeBoundsSafety.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InstrumentationRuntimeBoundsSafety.h"

#include "Plugins/Process/Utility/HistoryThread.h"
#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/InstrumentationRuntimeStopInfo.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/RegisterValue.h"
#include "lldb/Utility/RegularExpression.h"
#include "clang/CodeGen/ModuleBuilder.h"

#include <memory>

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(InstrumentationRuntimeBoundsSafety)

#define BOUNDS_SAFETY_SOFT_TRAP_MINIMAL "__bounds_safety_soft_trap"
#define BOUNDS_SAFETY_SOFT_TRAP_S "__bounds_safety_soft_trap_s"

std::vector<std::string> &getBoundsSafetySoftTrapRuntimeFuncs() {
  static std::vector<std::string> Funcs = {BOUNDS_SAFETY_SOFT_TRAP_MINIMAL,
                                           BOUNDS_SAFETY_SOFT_TRAP_S};

  return Funcs;
}

#define SOFT_TRAP_CATEGORY_PREFIX "Soft "
#define SOFT_TRAP_FALLBACK_CATEGORY                                            \
  SOFT_TRAP_CATEGORY_PREFIX "Bounds check failed"

class InstrumentationBoundsSafetyStopInfo : public StopInfo {
public:
  ~InstrumentationBoundsSafetyStopInfo() override = default;

  lldb::StopReason GetStopReason() const override {
    return lldb::eStopReasonInstrumentation;
  }

  std::optional<uint32_t>
  GetSuggestedStackFrameIndex(bool inlined_stack) override {
    return m_value;
  }

  const char *GetDescription() override { return m_description.c_str(); }

  bool DoShouldNotify(Event *event_ptr) override { return true; }

  static lldb::StopInfoSP
  CreateInstrumentationBoundsSafetyStopInfo(Thread &thread) {
    return StopInfoSP(new InstrumentationBoundsSafetyStopInfo(thread));
  }

private:
  std::pair<std::string, std::optional<uint32_t>>
  ComputeStopReasonAndSuggestedStackFrameWithDebugInfo(
      lldb::StackFrameSP parent_sf) {
    // First try to use debug info to understand the reason for trapping. The
    // call stack will look something like this:
    //
    // ```
    // frame #0: `__bounds_safety_soft_trap_s(reason="")
    // frame #1: `__clang_trap_msg$Bounds check failed$<reason>'
    // frame #2: `bad_read(index=10)
    // ```
    // ....
    const auto *TrapReasonFuncName = parent_sf->GetFunctionName();

    auto MaybeTrapReason =
        clang::CodeGen::DemangleTrapReasonInDebugInfo(TrapReasonFuncName);
    if (!MaybeTrapReason.has_value())
      return {};
    auto category = MaybeTrapReason.value().first;
    auto message = MaybeTrapReason.value().second;

    // TODO: Clang should probably be changed to emit the "Soft " prefix itself
    std::string stop_reason;
    llvm::raw_string_ostream ss(stop_reason);
    ss << SOFT_TRAP_CATEGORY_PREFIX;
    if (category.empty())
      ss << "<empty category>";
    else
      ss << category;
    if (!message.empty()) {
      ss << ": " << message;
    }
    // Use computed stop-reason and assume the parent of `parent_sf` is the
    // the place in the user's code where the call to the soft trap runtime
    // originated.
    return std::make_pair(stop_reason, parent_sf->GetFrameIndex() + 1);
  }

  std::pair<std::string, std::optional<uint32_t>>
  ComputeStopReasonAndSuggestedStackFrameWithoutDebugInfo(ThreadSP thread_sp) {
    auto softtrap_sf = thread_sp->GetStackFrameAtIndex(0);
    if (!softtrap_sf)
      return {};
    llvm::StringRef trap_reason_func_name = softtrap_sf->GetFunctionName();

    if (trap_reason_func_name == BOUNDS_SAFETY_SOFT_TRAP_MINIMAL) {
      // This function has no arguments so there's no additional information
      // that would allow us to identify the trap reason.
      return {};
    }

    // BOUNDS_SAFETY_SOFT_TRAP_S has one argument which is a pointer to a string
    // describing the trap or a nullptr.
    if (trap_reason_func_name != BOUNDS_SAFETY_SOFT_TRAP_S)
      return {};

    auto rc = thread_sp->GetRegisterContext();
    if (!rc)
      return {};

    // Don't try for architectures where examining the first register won't
    // work.
    auto process = thread_sp->GetProcess();
    if (!process)
      return {};
    switch (process->GetTarget().GetArchitecture().GetCore()) {
    case ArchSpec::eCore_x86_32_i386:
    case ArchSpec::eCore_x86_32_i486:
    case ArchSpec::eCore_x86_32_i486sx:
    case ArchSpec::eCore_x86_32_i686:
      // Technically some x86 calling conventions do use a register for
      // passing the first argument but let's ignore that for now.
      return {};
    default: {
    }
    };

    // Examine the register for the first argument
    auto *arg0_info = rc->GetRegisterInfo(
        lldb::RegisterKind::eRegisterKindGeneric, LLDB_REGNUM_GENERIC_ARG1);
    if (!arg0_info)
      return {};
    RegisterValue reg_value;
    if (!rc->ReadRegister(arg0_info, reg_value))
      return {};
    uint64_t reg_value_as_int = reg_value.GetAsUInt64(UINT64_MAX);
    if (reg_value_as_int == UINT64_MAX || reg_value_as_int == 0)
      return {};

    // The first argument to the call is a pointer to a global C string
    // containing the trap reason.
    std::string out_string;
    Status error_status;
    thread_sp->GetProcess()->ReadCStringFromMemory(reg_value_as_int, out_string,
                                                   error_status);
    if (error_status.Fail())
      return {};
    std::string stop_reason;
    llvm::raw_string_ostream SS(stop_reason);
    SS << SOFT_TRAP_FALLBACK_CATEGORY;
    if (!stop_reason.empty()) {
      SS << ": " << out_string;
    }
    // The frame where the call to the soft trap function happened is used
    // as the suggested frame. This should be frame where the bounds check
    // actually failed.
    return {stop_reason, 1};
  }

  std::pair<std::string, std::optional<uint32_t>>
  ComputeStopReasonAndSuggestedStackFrame() {

    ThreadSP thread_sp = GetThread();
    if (!thread_sp)
      return {};

    auto parent_sf = thread_sp->GetStackFrameAtIndex(1);
    if (!parent_sf)
      return {};

    if (parent_sf->HasDebugInformation()) {
      return ComputeStopReasonAndSuggestedStackFrameWithDebugInfo(parent_sf);
    }

    // If the debug info is missing we can still get some information
    // from the parameter in the soft trap runtime call.
    return ComputeStopReasonAndSuggestedStackFrameWithoutDebugInfo(thread_sp);
  }

  InstrumentationBoundsSafetyStopInfo(Thread &thread) : StopInfo(thread, 0) {
    // No additional data describing the reason for stopping
    m_extended_info = nullptr;
    m_description = SOFT_TRAP_FALLBACK_CATEGORY;

    auto [Description, MaybeSuggestedStackIndex] =
        ComputeStopReasonAndSuggestedStackFrame();
    if (Description.length() > 0)
      m_description = Description;
    if (MaybeSuggestedStackIndex)
      m_value = MaybeSuggestedStackIndex.value();
  }
};

InstrumentationRuntimeBoundsSafety::~InstrumentationRuntimeBoundsSafety() {
  Deactivate();
}

lldb::InstrumentationRuntimeSP
InstrumentationRuntimeBoundsSafety::CreateInstance(
    const lldb::ProcessSP &process_sp) {
  return InstrumentationRuntimeSP(
      new InstrumentationRuntimeBoundsSafety(process_sp));
}

void InstrumentationRuntimeBoundsSafety::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                "BoundsSafety instrumentation runtime plugin.",
                                CreateInstance, GetTypeStatic);
}

void InstrumentationRuntimeBoundsSafety::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb::InstrumentationRuntimeType
InstrumentationRuntimeBoundsSafety::GetTypeStatic() {
  return lldb::eInstrumentationRuntimeTypeBoundsSafety;
}

const RegularExpression &
InstrumentationRuntimeBoundsSafety::GetPatternForRuntimeLibrary() {
  static RegularExpression regex;
  return regex;
}

bool InstrumentationRuntimeBoundsSafety::CheckIfRuntimeIsValid(
    const lldb::ModuleSP module_sp) {

  for (const auto &SoftTrapFunc : getBoundsSafetySoftTrapRuntimeFuncs()) {
    ConstString test_sym(SoftTrapFunc);

    if (module_sp->FindFirstSymbolWithNameAndType(test_sym,
                                                  lldb::eSymbolTypeAny))
      return true;
  }
  return false;
}

bool InstrumentationRuntimeBoundsSafety::NotifyBreakpointHit(
    void *baton, StoppointCallbackContext *context, user_id_t break_id,
    user_id_t break_loc_id) {
  assert(baton && "null baton");
  if (!baton)
    return false; ///< false => resume execution.

  InstrumentationRuntimeBoundsSafety *const instance =
      static_cast<InstrumentationRuntimeBoundsSafety *>(baton);

  ProcessSP process_sp = instance->GetProcessSP();
  ThreadSP thread_sp = context->exe_ctx_ref.GetThreadSP();
  if (!process_sp || !thread_sp ||
      process_sp != context->exe_ctx_ref.GetProcessSP())
    return false;

  if (process_sp->GetModIDRef().IsLastResumeForUserExpression())
    return false;

  thread_sp->SetStopInfo(
      InstrumentationBoundsSafetyStopInfo::
          CreateInstrumentationBoundsSafetyStopInfo(*thread_sp));
  return true;
}

void InstrumentationRuntimeBoundsSafety::Activate() {
  if (IsActive())
    return;

  ProcessSP process_sp = GetProcessSP();
  if (!process_sp)
    return;

  auto breakpoint = process_sp->GetTarget().CreateBreakpoint(
      /*containingModules=*/nullptr,
      /*containingSourceFiles=*/nullptr, getBoundsSafetySoftTrapRuntimeFuncs(),
      eFunctionNameTypeFull, eLanguageTypeUnknown,
      /*m_offset=*/0,
      /*skip_prologue*/ eLazyBoolNo,
      /*internal=*/true,
      /*request_hardware*/ false);

  // FIXME: If I use `sync=true` this breaks... why?
  breakpoint->SetCallback(
      InstrumentationRuntimeBoundsSafety::NotifyBreakpointHit, this,
      /*sync=*/false);
  breakpoint->SetBreakpointKind("bounds-safety-soft-trap");
  SetBreakpointID(breakpoint->GetID());
  SetActive(true);
}

void InstrumentationRuntimeBoundsSafety::Deactivate() {
  SetActive(false);
  if (ProcessSP process_sp = GetProcessSP())
    process_sp->GetTarget().RemoveBreakpointByID(GetBreakpointID());

  SetBreakpointID(LLDB_INVALID_BREAK_ID);
}

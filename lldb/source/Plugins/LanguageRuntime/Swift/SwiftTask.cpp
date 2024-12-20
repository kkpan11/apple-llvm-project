#include "SwiftTask.h"
#include "SwiftLanguageRuntime.h"
#include "lldb/Target/Process.h"

using namespace llvm;
using namespace lldb;

lldb_private::ThreadTask::ThreadTask(tid_t tid, addr_t async_ctx,
                                     ExecutionContext &exe_ctx)
    : Thread(exe_ctx.GetProcessRef(), tid, true),
      m_concrete_reg_ctx_sp(exe_ctx.GetFrameSP()->GetRegisterContext()) {
  m_async_ctx = async_ctx;
  auto ptr_size = exe_ctx.GetTargetRef().GetArchitecture().GetAddressByteSize();
  // A simplified description of AsyncContext. See swift/Task/ABI.h
  // struct AsyncContext {
  //   AsyncContext *Parent;                    // offset 0
  //   TaskContinuationFunction *ResumeParent;  // offset 8
  // };
  auto resume_offset = ptr_size; // offsetof(AsyncContext, ResumeParent)
  auto resume_ptr = async_ctx + resume_offset;
  Status status;
  m_pc = exe_ctx.GetProcessRef().ReadPointerFromMemory(resume_ptr, status);
}

RegisterContextSP lldb_private::ThreadTask::GetRegisterContext() {
  if (!m_async_reg_ctx_sp)
    m_async_reg_ctx_sp = std::make_shared<RegisterContextTask>(
        *this, m_concrete_reg_ctx_sp, m_pc, m_async_ctx);
  return m_async_reg_ctx_sp;
}

lldb_private::RegisterContextTask::RegisterContextTask(
    Thread &thread, RegisterContextSP reg_info_sp, addr_t pc, addr_t async_ctx)
    : RegisterContext(thread, 0), m_reg_info_sp(reg_info_sp),
      m_async_ctx(async_ctx), m_pc(pc) {
  auto &target = thread.GetProcess()->GetTarget();
  auto triple = target.GetArchitecture().GetTriple();
  if (auto regnums = GetAsyncUnwindRegisterNumbers(triple.getArch()))
    m_async_ctx_regnum = regnums->async_ctx_regnum;
}

bool lldb_private::RegisterContextTask::ReadRegister(
    const RegisterInfo *reg_info, RegisterValue &reg_value) {
  if (reg_info->kinds[eRegisterKindGeneric] == LLDB_REGNUM_GENERIC_PC) {
    reg_value = m_pc;
    return true;
  }
  if (reg_info->kinds[eRegisterKindLLDB] == m_async_ctx_regnum) {
    reg_value = m_async_ctx;
    return true;
  }
  return false;
}

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_FRONTEND_H_
#define XENIA_CPU_PPC_PPC_FRONTEND_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include "xenia/base/type_pool.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/guest_code_reader.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace ppc {

class PPCTranslator;

struct GuestSystemCallEvent {
  // Address of the instruction that emitted the event.
  uint32_t cia;
  // cia + 4, wrapping at 32 bits.
  uint32_t next_cia;
  uint64_t number;
};

// Program interrupts raised by checked-domain code. DefaultDirect code skips
// invalid instructions and handles traps per ignore_trap_instructions.
enum class GuestProgramExceptionKind : uint32_t {
  // An invalid opcode or an invalid form the processor rejects.
  kIllegalInstruction = 1,
  // A tw/twi/td/tdi whose condition held.
  kTrap = 2,
};

struct GuestProgramExceptionEvent {
  // Address of the instruction that raised the exception.
  uint32_t cia;
  GuestProgramExceptionKind kind;
};

// Handles guest LEV-0 system calls; returning false takes the unhandled-syscall
// path. Must outlive its registration.
class GuestSyscallDispatcher {
 public:
  virtual ~GuestSyscallDispatcher() = default;
  virtual bool DispatchGuestSyscall(PPCContext* ppc_context,
                                    const GuestSystemCallEvent& event) = 0;
  // Returning false leaves the instruction without effect.
  virtual bool DispatchGuestProgramException(
      PPCContext* ppc_context, const GuestProgramExceptionEvent& event) {
    return false;
  }
};

struct PPCBuiltins {
  int32_t global_lock_count;
  Function* check_global_lock;
  Function* enter_global_lock;
  Function* leave_global_lock;
  Function* syscall_handler;
  Function* illegal_instruction_handler;
  Function* trap_handler;
  // Takes a pending interrupt at a checked-domain backward branch; the
  // branch target is in guest_system_call_cia.
  Function* interrupt_poll_handler;
};

class PPCFrontend {
 public:
  explicit PPCFrontend(Processor* processor);
  ~PPCFrontend();

  bool Initialize();

  Processor* processor() const { return processor_; }
  Memory* memory() const;
  PPCBuiltins* builtins() { return &builtins_; }

  // The instruction reader of a translation domain, or null when the domain
  // is unknown or retired.
  const GuestCodeReader* guest_code_reader(
      TranslationDomainKey translation_domain_key) const;

  void set_guest_syscall_dispatcher(GuestSyscallDispatcher* dispatcher) {
    guest_syscall_dispatcher_.store(dispatcher, std::memory_order_release);
  }
  GuestSyscallDispatcher* guest_syscall_dispatcher() const {
    return guest_syscall_dispatcher_.load(std::memory_order_acquire);
  }
  bool DispatchGuestSyscall(PPCContext* ppc_context,
                            const GuestSystemCallEvent& event) const;
  bool DispatchGuestProgramException(
      PPCContext* ppc_context, const GuestProgramExceptionEvent& event) const;

  bool DeclareFunction(GuestFunction* function);
  bool DefineFunction(GuestFunction* function, uint32_t debug_info_flags);

 private:
  Processor* processor_;
  MemoryGuestCodeReader guest_code_reader_;
  PPCBuiltins builtins_ = {0};
  std::atomic<GuestSyscallDispatcher*> guest_syscall_dispatcher_{nullptr};
  TypePool<PPCTranslator, PPCFrontend*> translator_pool_;
};
// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1);
}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_FRONTEND_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_frontend.h"

#include "xenia/base/atomic.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_emit.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace ppc {

void InitializeIfNeeded();
void CleanupOnShutdown();

void InitializeIfNeeded() {
  static bool has_initialized = false;
  if (has_initialized) {
    return;
  }
  has_initialized = true;

  RegisterEmitCategoryAltivec();
  RegisterEmitCategoryALU();
  RegisterEmitCategoryControl();
  RegisterEmitCategoryFPU();
  RegisterEmitCategoryMemory();

  atexit(CleanupOnShutdown);
}

void CleanupOnShutdown() {}

PPCFrontend::PPCFrontend(Processor* processor)
    : processor_(processor),
      guest_code_reader_(processor ? processor->memory() : nullptr) {
  InitializeIfNeeded();
}

PPCFrontend::~PPCFrontend() {
  // Force cleanup now before we deinit.
  translator_pool_.Reset();
}

Memory* PPCFrontend::memory() const { return processor_->memory(); }

const GuestCodeReader* PPCFrontend::guest_code_reader(
    TranslationDomainKey translation_domain_key) const {
  if (translation_domain_key == TranslationDomainKey::DefaultDirect()) {
    return &guest_code_reader_;
  }
  if (!processor_) {
    return nullptr;
  }
  const TranslationDomainDescriptor* descriptor =
      processor_->translation_domain_registry()->Lookup(translation_domain_key);
  return descriptor ? descriptor->guest_code_reader() : nullptr;
}

// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  std::lock_guard<global_mutex_type> lock(*global_mutex);
  ppc_context->scratch = *global_lock_count ? 0 : 0x8000;
}

// Enters the global lock. Safe to recursion.
void EnterGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  global_mutex->lock();
  xe::atomic_inc(global_lock_count);
}

// Leaves the global lock. Safe to recursion.
void LeaveGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  auto new_lock_count = xe::atomic_dec(global_lock_count);
  assert_true(new_lock_count >= 0);
  global_mutex->unlock();
}

void SyscallHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto frontend = reinterpret_cast<PPCFrontend*>(arg0);
  const uint32_t cia = ppc_context->guest_system_call_cia;
  const GuestSystemCallEvent event = {
      cia, static_cast<uint32_t>(cia + uint32_t{4}), ppc_context->r[0]};
  if (frontend && frontend->DispatchGuestSyscall(ppc_context, event)) {
    return;
  }

  uint64_t syscall_number = event.number;
  switch (syscall_number) {
    default:
      assert_unhandled_case(syscall_number);
      XELOGE("Unhandled syscall {}!", syscall_number);
      break;
#pragma warning(suppress : 4065)
  }
}

void ProgramExceptionHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto frontend = reinterpret_cast<PPCFrontend*>(arg0);
  const GuestProgramExceptionEvent event = {
      ppc_context->guest_system_call_cia,
      static_cast<GuestProgramExceptionKind>(
          reinterpret_cast<uintptr_t>(arg1))};
  if (frontend && frontend->DispatchGuestProgramException(ppc_context, event)) {
    return;
  }
  XELOGE("Unhandled guest program exception {} at {:08X}",
         static_cast<uint32_t>(event.kind), event.cia);
}

void InterruptPollHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  (void)arg1;
  auto frontend = reinterpret_cast<PPCFrontend*>(arg0);
  GuestInterruptPoller* poller =
      frontend ? frontend->processor()->guest_interrupt_poller() : nullptr;
  if (poller) {
    // May leave nonlocally and resume at the branch target later.
    poller->PollGuestInterrupt(ppc_context, ppc_context->guest_system_call_cia);
  }
}

bool PPCFrontend::DispatchGuestProgramException(
    PPCContext* ppc_context, const GuestProgramExceptionEvent& event) const {
  GuestSyscallDispatcher* dispatcher = guest_syscall_dispatcher();
  return dispatcher &&
         dispatcher->DispatchGuestProgramException(ppc_context, event);
}

bool PPCFrontend::DispatchGuestSyscall(
    PPCContext* ppc_context, const GuestSystemCallEvent& event) const {
  GuestSyscallDispatcher* dispatcher = guest_syscall_dispatcher();
  return dispatcher && dispatcher->DispatchGuestSyscall(ppc_context, event);
}

bool PPCFrontend::Initialize() {
  void* arg0 = reinterpret_cast<void*>(&xe::global_critical_region::mutex());
  void* arg1 = reinterpret_cast<void*>(&builtins_.global_lock_count);
  builtins_.check_global_lock =
      processor_->DefineBuiltin("CheckGlobalLock", CheckGlobalLock, arg0, arg1);
  builtins_.enter_global_lock =
      processor_->DefineBuiltin("EnterGlobalLock", EnterGlobalLock, arg0, arg1);
  builtins_.leave_global_lock =
      processor_->DefineBuiltin("LeaveGlobalLock", LeaveGlobalLock, arg0, arg1);
  builtins_.syscall_handler = processor_->DefineBuiltin(
      "SyscallHandler", SyscallHandler, this, nullptr);
  builtins_.illegal_instruction_handler = processor_->DefineBuiltin(
      "IllegalInstructionHandler", ProgramExceptionHandler, this,
      reinterpret_cast<void*>(static_cast<uintptr_t>(
          GuestProgramExceptionKind::kIllegalInstruction)));
  builtins_.interrupt_poll_handler = processor_->DefineBuiltin(
      "InterruptPollHandler", InterruptPollHandler, this, nullptr);
  builtins_.trap_handler = processor_->DefineBuiltin(
      "TrapHandler", ProgramExceptionHandler, this,
      reinterpret_cast<void*>(
          static_cast<uintptr_t>(GuestProgramExceptionKind::kTrap)));
  return true;
}

bool PPCFrontend::DeclareFunction(GuestFunction* function) {
  if (!function || !processor_->CanExecuteTranslationDomain(
                       function->translation_domain_key())) {
    XELOGE(
        "Refusing to declare guest function in an unsupported translation "
        "domain");
    return false;
  }

  // chrispy: make sure we aren't declaring a function that is actually padding
  // data, this will mess up PPCScanner and is hard to debug wow, this halo
  // reach actually has branches into 0 opcodes, look into further
  // xenia_assert(*reinterpret_cast<const uint32_t*>(
  //                  this->memory()->TranslateVirtual(function->address())) !=
  //                  0);
  //  Could scan or something here.
  //  Could also check to see if it's a well-known function type and classify
  //  for later.
  //  Could also kick off a precompiler, since we know it's likely the function
  //  will be demanded soon.
  return true;
}

bool PPCFrontend::DefineFunction(GuestFunction* function,
                                 uint32_t debug_info_flags) {
  if (!function || !processor_->CanExecuteTranslationDomain(
                       function->translation_domain_key())) {
    XELOGE(
        "Refusing to define guest function in an unsupported translation "
        "domain");
    return false;
  }

  auto translator = translator_pool_.Allocate(this);
  bool result = translator->Translate(function, debug_info_flags);
  translator->Reset();
  translator_pool_.Release(translator);
  return result;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

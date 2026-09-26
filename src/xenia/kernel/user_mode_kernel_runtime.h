/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_KERNEL_RUNTIME_H_
#define XENIA_KERNEL_USER_MODE_KERNEL_RUNTIME_H_

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "xenia/cpu/guest_memory_access.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/user_mode_manager.h"
#include "xenia/xbox.h"

namespace xe::kernel {

class KernelState;
class UserModeGuestCodeDomainCoordinator;
class UserModeGuestCodeDomainLease;
class XThread;

enum class UserModePendingKernelAction : uint8_t {
  kNone,
  kHandler,
  kFlush,
  // Queued DPCs interrupt user-mode code; it resumes at the branch target.
  kInterrupt,
};

// The exception record of a checked-memory fault.
X_EXCEPTION_RECORD BuildUserModeMemoryExceptionRecord(
    const cpu::GuestMemoryAccessRequest& request,
    const cpu::GuestMemoryAccessResult& result);

// An instruction storage fault is an access violation reading its target (a
// nonzero first parameter would mean a write).
X_EXCEPTION_RECORD BuildUserModeInstructionStorageExceptionRecord(
    const cpu::ppc::GuestInstructionFetchFault& fault);

// Other threads mark a thread's trap frame (named by KTHREAD+0x128) by
// setting or clearing CR1 bits. Returns cr with the CR1 bits that changed
// between written_cr (the frame's CR as last written or applied) and frame_cr
// (its CR now).
uint32_t ApplyTrapFrameConditionRegisterMarks(uint32_t cr, uint32_t frame_cr,
                                              uint32_t written_cr);

// Where a handler's return resumes the user context: the trap frame's IAR, or
// nothing when it is 0, which the console would fault on again.
std::optional<uint32_t> SelectUserModeHandlerResumeAddress(
    const UserModeIntegerContext& frame);

// The handler runs with the outer (kernel) state, except that r14-r31 keep the
// user's values.
void ApplyUserModeHandlerEntryState(const UserModeOuterIntegerState& outer,
                                    const UserModeIntegerContext& user,
                                    cpu::ppc::PPCContext& context);

// Exception record for a user-mode program interrupt at instruction_address;
// illegal instructions and traps are both STATUS_ILLEGAL_INSTRUCTION.
X_EXCEPTION_RECORD BuildUserModeProgramExceptionRecord(
    uint32_t instruction_address, cpu::ppc::GuestProgramExceptionKind kind);

// Sets the user-mode MSR: problem state, with UserModeControl bit 2 as LE.
// Replaces the caller's MSR.
void ApplyRetailUserModeMachineState(uint32_t user_mode_control,
                                     cpu::ppc::PPCContext& ppc_context);

// Restores what KeLeaveUserMode restores: r1, r14-r31, LR and CR. r3 keeps the
// handler's return value; CTR, XER and the MSR are kept.
void ApplyRetailUserModeLeaveState(const UserModeOuterIntegerState& outer_state,
                                   cpu::ppc::PPCContext& ppc_context);

// Implements the kernel's user-mode services, LEV-0 system calls and
// checked-memory faults. Domain switches are staged into XThread and performed
// once owning locals and locks are out of scope: longjmp reentry skips
// destructors.
class UserModeKernelRuntime final : public cpu::ppc::GuestSyscallDispatcher,
                                    public cpu::CheckedGuestMemoryFaultHandler,
                                    public cpu::GuestInterruptPoller {
 public:
  explicit UserModeKernelRuntime(KernelState* kernel_state);
  ~UserModeKernelRuntime() override;

  UserModeKernelRuntime(const UserModeKernelRuntime&) = delete;
  UserModeKernelRuntime& operator=(const UserModeKernelRuntime&) = delete;

  bool is_ready() const { return is_ready_; }

  X_STATUS CreateMapping(uint32_t selector,
                         uint32_t control_page_virtual_address);
  void DeleteMapping(uint32_t selector);

  // Returns only on failure; success enters the checked domain nonlocally.
  X_STATUS Enter(uint32_t context_virtual_address, uint32_t handler_address,
                 uint32_t save_area_end, uint32_t companion_state,
                 cpu::ppc::PPCContext* ppc_context);

  // Returns when called from DefaultDirect. A checked-domain caller is unwound
  // into the runtime continuation, flushed there, then resumed at its LR.
  void Flush(uint32_t selector, cpu::ppc::PPCContext* ppc_context);
  // KeFlushCurrentEntireTb: flushes the translations of every mapping.
  void FlushEntireTb(cpu::ppc::PPCContext* ppc_context);

  // Called from the custom handler: restores the outer state and resumes at
  // its LR, KeEnterUserMode's return address, through a reentry.
  [[noreturn]] void Leave(cpu::ppc::PPCContext* ppc_context);

  bool DispatchGuestSyscall(
      cpu::ppc::PPCContext* ppc_context,
      const cpu::ppc::GuestSystemCallEvent& event) override;

  bool DispatchGuestProgramException(
      cpu::ppc::PPCContext* ppc_context,
      const cpu::ppc::GuestProgramExceptionEvent& event) override;

  [[noreturn]] void HandleGuestMemoryFault(
      cpu::ppc::PPCContext* ppc_context,
      cpu::TranslationDomainKey translation_domain_key,
      const cpu::GuestMemoryAccessRequest& request,
      const cpu::GuestMemoryAccessResult& result) override;

  // Delivers the fault to the mapping's handler; returns false only when it is
  // not this thread's checked-domain fault.
  bool DispatchGuestInstructionStorageFault(
      cpu::ppc::PPCContext* ppc_context,
      cpu::TranslationDomainKey translation_domain_key,
      const cpu::ppc::GuestInstructionFetchFault& fault) override;

  // Queues a DPC that targets thread_id's processor: while the thread runs
  // user-mode code, the DPC interrupts it at its next branch and runs on it,
  // never concurrently with it. Returns false when the thread has no
  // user-mode state; the caller runs the DPC elsewhere.
  bool QueueThreadDpc(uint32_t thread_id, uint32_t dpc_ptr, uint32_t arg1,
                      uint32_t arg2);
  // As QueueThreadDpc, for the user-mode thread running on processor cpu.
  // Returns false when there is none.
  bool QueueProcessorDpc(uint8_t cpu, uint32_t dpc_ptr, uint32_t arg1,
                         uint32_t arg2);
  void PollGuestInterrupt(cpu::ppc::PPCContext* ppc_context,
                          uint32_t resume_address) override;

  void OnThreadDestroyed(uint32_t thread_id);
  void ResetTitle();

  size_t active_thread_state_count() const;

 private:
  struct PendingDpc {
    uint32_t dpc_ptr;
    uint32_t arg1;
    uint32_t arg2;
  };

  struct ThreadRuntimeState {
    UserModeMappingHandle mapping;
    cpu::ThreadState* thread_state = nullptr;
    // The thread's KPCR, whose PRCB names the processor it runs on.
    uint32_t pcr_address = 0;
    // The thread's KTHREAD. Its trap-frame pointer (KTHREAD+0x128) names the
    // handler block's trap frame while the thread is in user mode.
    uint32_t kthread_address = 0;
    // The trap frame's CR as the runtime last wrote or applied it.
    uint32_t trap_frame_condition_register = 0;
    std::vector<PendingDpc> pending_dpcs;
    // Bytes of the PPCContext an interrupt saved, restored on resume.
    std::vector<uint8_t> interrupted_context;
    uint32_t context_virtual_address = 0;
    uint32_t exception_record_virtual_address = 0;
    UserModePendingKernelAction pending_action =
        UserModePendingKernelAction::kNone;
    UserModeMappingHandle pending_flush_mapping;
    uint64_t pending_frame_revision = 0;
    uint32_t pending_resume_address = 0;
  };

  static void ContinuationThunk(cpu::ppc::PPCContext* ppc_context, void* arg0,
                                void* arg1);
  // Applies to ppc_context the CR1 bits other threads changed in the thread's
  // trap frame since the runtime last wrote or applied it. Requires mutex_.
  void ApplyTrapFrameMarksLocked(ThreadRuntimeState& state,
                                 cpu::ppc::PPCContext& ppc_context);
  // Names the thread's trap frame in its KTHREAD, or clears the name.
  // Requires mutex_.
  void PublishTrapFrameLocked(const ThreadRuntimeState& state, bool publish);
  // Requires mutex_.
  static void QueuePendingDpc(ThreadRuntimeState& state, uint32_t dpc_ptr,
                              uint32_t arg1, uint32_t arg2);
  [[noreturn]] void Continue(cpu::ppc::PPCContext* ppc_context);
  [[noreturn]] void FailInvariant(const char* operation,
                                  cpu::ppc::PPCContext* ppc_context);
  // Saves the interrupted state, writes exception_record and the trap frame,
  // and enters the mapping handler. The handler resumes through the trap
  // frame, whose IAR starts as resume_address.
  [[noreturn]] void DeliverExceptionToHandler(
      XThread& thread, cpu::ppc::PPCContext* ppc_context,
      cpu::TranslationDomainKey translation_domain_key, uint32_t resume_address,
      const X_EXCEPTION_RECORD& exception_record, const char* operation);

  bool StageTransition(XThread& thread, UserModeMappingHandle mapping,
                       cpu::TranslationDomainKey expected_source_domain_key,
                       cpu::TranslationDomainKey target_domain_key);
  static cpu::TranslationDomainKey SelectDomain(
      const UserModeGuestCodeDomainLease& lease, uint32_t user_mode_control);
  bool ClearThreadRuntimeStateLocked(
      uint32_t thread_id, uint32_t* exception_record_out,
      std::vector<PendingDpc>* dpcs_out = nullptr);
  // Runs the DPCs an interrupt took and resumes the interrupted code.
  [[noreturn]] void ReturnFromInterrupt(XThread& thread,
                                        cpu::ppc::PPCContext* ppc_context);

  KernelState* const kernel_state_;
  UserModeManager* const manager_;
  UserModeGuestCodeDomainCoordinator* const coordinator_;
  cpu::Function* continuation_function_ = nullptr;
  // The return address user-mode handlers get: a guest trampoline into the
  // continuation, since a blr can't reach a builtin address.
  uint32_t handler_return_address_ = 0;
  bool frontend_dispatcher_installed_ = false;
  bool fault_handler_installed_ = false;
  bool is_ready_ = false;

  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, ThreadRuntimeState> thread_states_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_KERNEL_RUNTIME_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_kernel_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/guest_memory_access.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_mode_context.h"
#include "xenia/kernel/user_mode_context_snapshot.h"
#include "xenia/kernel/user_mode_create.h"
#include "xenia/kernel/user_mode_enter.h"
#include "xenia/kernel/user_mode_guest_code_domains.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xefu_link.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

constexpr X_STATUS kStatusDatatypeMisalignment = 0x80000002u;
constexpr uint64_t kUserModeMsrProblemState = 0x4000;
constexpr uint64_t kUserModeMsrLittleEndian = 0x1;

// Each user-mode thread owns one guest block holding the exception record and,
// after it, the trap frame passed to its handler.
constexpr uint32_t kHandlerTrapFrameOffset =
    (uint32_t(sizeof(X_EXCEPTION_RECORD)) + 0xF) & ~uint32_t(0xF);
constexpr uint32_t kHandlerBlockSize =
    kHandlerTrapFrameOffset + uint32_t(kUserModeTrapFrameSize);

uint32_t HandlerTrapFrameAddress(uint32_t handler_block) {
  return handler_block + kHandlerTrapFrameOffset;
}

// The handler reads and edits the interrupted state through the trap frame,
// and the thread resumes from it.
bool WriteHandlerTrapFrame(Memory& memory, uint32_t handler_block,
                           const UserModeIntegerContext& context) {
  auto* frame =
      memory.TranslateVirtual<uint8_t*>(HandlerTrapFrameAddress(handler_block));
  return EncodeUserModeTrapFrame(
             context, std::span<uint8_t>(frame, kUserModeTrapFrameSize)) ==
         UserModeContextCodecError::kNone;
}

// Other threads mark a thread's trap frame while it runs user-mode code (on the
// console, only while it is interrupted), so the frame stays named in user mode
// and the marks apply when the thread enters the kernel.
constexpr uint32_t kTrapFrameMarkMask = 0x0F000000;  // CR1

uint32_t ReadTrapFrameConditionRegister(Memory& memory,
                                        uint32_t handler_block) {
  return xe::load_and_swap<uint32_t>(
      memory.TranslateVirtual<const uint8_t*>(
          HandlerTrapFrameAddress(handler_block)) +
      kUserModeTrapFrameConditionRegisterOffset);
}

bool IsCheckedDomain(cpu::TranslationDomainKey domain) {
  return domain.id() != cpu::TranslationDomainKey::kDefaultDirectId &&
         domain.memory_access_mode() == cpu::MemoryAccessMode::kChecked;
}

bool IsWriteOperation(cpu::GuestMemoryAccessOperation operation) {
  switch (operation) {
    case cpu::GuestMemoryAccessOperation::kStore:
    case cpu::GuestMemoryAccessOperation::kStoreVectorLeft:
    case cpu::GuestMemoryAccessOperation::kStoreVectorRight:
    case cpu::GuestMemoryAccessOperation::kMemset:
    case cpu::GuestMemoryAccessOperation::kAtomicCompareExchange:
    case cpu::GuestMemoryAccessOperation::kStoreConditional:
      return true;
    case cpu::GuestMemoryAccessOperation::kLoad:
    case cpu::GuestMemoryAccessOperation::kLoadVectorLeft:
    case cpu::GuestMemoryAccessOperation::kLoadVectorRight:
    case cpu::GuestMemoryAccessOperation::kLoadReserve:
      return false;
  }
  return false;
}

}  // namespace

X_EXCEPTION_RECORD BuildUserModeMemoryExceptionRecord(
    const cpu::GuestMemoryAccessRequest& request,
    const cpu::GuestMemoryAccessResult& result) {
  X_EXCEPTION_RECORD record{};
  record.exception_flags = 0;
  record.exception_record = 0;
  record.exception_address = request.instruction_address;

  if (result.fault.reason == cpu::GuestMemoryAccessFaultReason::kAlignment) {
    // The handler emulates the access at this effective address.
    record.code = kStatusDatatypeMisalignment;
    record.number_parameters = 1;
    record.exception_information[0] = result.fault.effective_address;
    return record;
  }

  record.code = X_STATUS_ACCESS_VIOLATION;
  record.number_parameters = 2;
  record.exception_information[0] = IsWriteOperation(request.operation) ? 1 : 0;
  record.exception_information[1] = result.fault.effective_address;
  return record;
}

uint32_t ApplyTrapFrameConditionRegisterMarks(uint32_t cr, uint32_t frame_cr,
                                              uint32_t written_cr) {
  const uint32_t set = frame_cr & ~written_cr & kTrapFrameMarkMask;
  const uint32_t cleared = written_cr & ~frame_cr & kTrapFrameMarkMask;
  return (cr | set) & ~cleared;
}

X_EXCEPTION_RECORD BuildUserModeInstructionStorageExceptionRecord(
    const cpu::ppc::GuestInstructionFetchFault& fault) {
  X_EXCEPTION_RECORD record{};
  record.code = X_STATUS_ACCESS_VIOLATION;
  record.exception_flags = 0;
  record.exception_record = 0;
  record.exception_address = fault.instruction_address;
  record.number_parameters = 2;
  record.exception_information[0] = 0;
  record.exception_information[1] = fault.instruction_address;
  return record;
}

std::optional<uint32_t> SelectUserModeHandlerResumeAddress(
    const UserModeIntegerContext& frame) {
  if (!frame.entry_address) {
    return std::nullopt;
  }
  return frame.entry_address;
}

void ApplyUserModeHandlerEntryState(const UserModeOuterIntegerState& outer,
                                    const UserModeIntegerContext& user,
                                    cpu::ppc::PPCContext& context) {
  ApplyUserModeOuterIntegerState(outer, context);
  std::copy(user.gpr.begin() + 14, user.gpr.end(), std::begin(context.r) + 14);
}

X_EXCEPTION_RECORD BuildUserModeProgramExceptionRecord(
    uint32_t instruction_address, cpu::ppc::GuestProgramExceptionKind kind) {
  (void)kind;
  X_EXCEPTION_RECORD record{};
  record.code = X_STATUS_ILLEGAL_INSTRUCTION;
  record.exception_flags = 0;
  record.exception_record = 0;
  record.exception_address = instruction_address;
  record.number_parameters = 0;
  return record;
}

void ApplyRetailUserModeMachineState(uint32_t user_mode_control,
                                     cpu::ppc::PPCContext& ppc_context) {
  const UserModeControlSelection selection =
      DecodeUserModeControlSelection(user_mode_control);
  ppc_context.msr = kUserModeMsrProblemState |
                    (selection.little_endian ? kUserModeMsrLittleEndian : 0);
}

void ApplyRetailUserModeLeaveState(const UserModeOuterIntegerState& outer_state,
                                   cpu::ppc::PPCContext& ppc_context) {
  ppc_context.r[1] = outer_state.gpr[1];
  std::copy(outer_state.gpr.cbegin() + 14, outer_state.gpr.cend(),
            ppc_context.r + 14);
  ppc_context.lr = outer_state.link_register;
  ppc_context.set_cr(outer_state.condition_register);
}

UserModeKernelRuntime::UserModeKernelRuntime(KernelState* kernel_state)
    : kernel_state_(kernel_state),
      manager_(kernel_state ? kernel_state->user_mode_manager() : nullptr),
      coordinator_(kernel_state
                       ? kernel_state->user_mode_guest_code_domain_coordinator()
                       : nullptr) {
  if (!kernel_state_ || !manager_ || !coordinator_ ||
      manager_->mode() != UserModeManagerMode::kExecutionEnabled ||
      coordinator_->mode() !=
          UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled) {
    return;
  }

  cpu::Processor* processor = kernel_state_->processor();
  cpu::ppc::PPCFrontend* frontend = processor ? processor->frontend() : nullptr;
  if (!processor || !frontend || frontend->guest_syscall_dispatcher()) {
    return;
  }

  continuation_function_ = processor->DefineBuiltin(
      "UserModeKernelContinuation", ContinuationThunk, this, nullptr);
  if (!continuation_function_) {
    return;
  }
  // Builtin addresses sit outside the indirection-table window, so a guest
  // handler's blr cannot reach continuation_function_ directly.
  if (cpu::backend::Backend* backend = processor->backend()) {
    handler_return_address_ =
        backend->CreateGuestTrampoline(ContinuationThunk, this, nullptr, true);
  }
  if (!handler_return_address_) {
    XELOGE(
        "XeFU user-mode runtime could not create a handler return "
        "trampoline");
    return;
  }

  frontend->set_guest_syscall_dispatcher(this);
  frontend_dispatcher_installed_ = true;
  processor->set_guest_interrupt_poller(this);
  fault_handler_installed_ =
      coordinator_->InstallCheckedGuestMemoryFaultHandler(this);
  if (!fault_handler_installed_) {
    frontend->set_guest_syscall_dispatcher(nullptr);
    frontend_dispatcher_installed_ = false;
    processor->backend()->FreeGuestTrampoline(handler_return_address_);
    handler_return_address_ = 0;
    return;
  }
  is_ready_ = true;
}

UserModeKernelRuntime::~UserModeKernelRuntime() {
  is_ready_ = false;
  ResetTitle();
  if (fault_handler_installed_) {
    coordinator_->UninstallCheckedGuestMemoryFaultHandler(this);
    fault_handler_installed_ = false;
  }
  if (kernel_state_->processor()->guest_interrupt_poller() == this) {
    kernel_state_->processor()->set_guest_interrupt_poller(nullptr);
  }
  if (frontend_dispatcher_installed_) {
    cpu::ppc::PPCFrontend* frontend = kernel_state_->processor()->frontend();
    if (frontend && frontend->guest_syscall_dispatcher() == this) {
      frontend->set_guest_syscall_dispatcher(nullptr);
    }
    frontend_dispatcher_installed_ = false;
  }
  if (handler_return_address_) {
    kernel_state_->processor()->backend()->FreeGuestTrampoline(
        handler_return_address_);
    handler_return_address_ = 0;
  }
}

X_STATUS UserModeKernelRuntime::CreateMapping(
    uint32_t selector, uint32_t control_page_virtual_address) {
  if (!is_ready_) {
    return X_STATUS_NOT_SUPPORTED;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  UserModeCreateFromGuestPageResult create = CreateUserModeMappingFromGuestPage(
      *kernel_state_->memory(), *manager_, selector,
      control_page_virtual_address);
  if (!create.mapping.handle) {
    return create.mapping.guest_status.value_or(X_STATUS_UNSUCCESSFUL);
  }

  const UserModeMappingHandle handle = *create.mapping.handle;
  std::optional<UserModeMappingSnapshot> snapshot =
      manager_->FindMapping(handle);
  if (!snapshot) {
    manager_->DeleteMapping(handle);
    return X_STATUS_UNSUCCESSFUL;
  }

  UserModeGuestCodeDomainCoordinatorResult registration =
      coordinator_->RegisterMapping(*snapshot);
  if (!registration || !registration.execution_permitted) {
    manager_->DeleteMapping(handle);
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

void UserModeKernelRuntime::DeleteMapping(uint32_t selector) {
  if (!is_ready_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<UserModeMappingSnapshot> snapshot =
      manager_->FindMappingBySelector(selector);
  if (!snapshot) {
    XELOGW("KeDeleteUserMode ignored missing selector {}", selector);
    return;
  }
  const UserModeManagerDiagnostic diagnostic =
      manager_->DeleteMapping(snapshot->handle);
  if (diagnostic != UserModeManagerDiagnostic::kNone) {
    XELOGE("KeDeleteUserMode rejected selector {} (diagnostic {})", selector,
           static_cast<uint32_t>(diagnostic));
  }
}

cpu::TranslationDomainKey UserModeKernelRuntime::SelectDomain(
    const UserModeGuestCodeDomainLease& lease, uint32_t user_mode_control) {
  const UserModeControlSelection selection =
      DecodeUserModeControlSelection(user_mode_control);
  return lease.domains()[selection.little_endian ? 1 : 0][selection.slb_half];
}

bool UserModeKernelRuntime::StageTransition(
    XThread& thread, UserModeMappingHandle mapping,
    cpu::TranslationDomainKey expected_source_domain_key,
    cpu::TranslationDomainKey target_domain_key) {
  UserModeGuestCodeDomainTransitionResult transition =
      coordinator_->AcquireTransition(mapping, expected_source_domain_key,
                                      target_domain_key);
  if (!transition) {
    return false;
  }
  return thread.StageTranslationDomainReentry(target_domain_key,
                                              std::move(transition.permit));
}

X_STATUS UserModeKernelRuntime::Enter(uint32_t context_virtual_address,
                                      uint32_t handler_address,
                                      uint32_t save_area_end,
                                      uint32_t companion_state,
                                      cpu::ppc::PPCContext* ppc_context) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context ||
      ppc_context->thread_id != thread->thread_id()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Reject a zero handler and reentry before reading the context; the manager
  // checks again when it publishes.
  if (!handler_address || manager_->FindActiveFrame(thread->thread_id())) {
    return X_STATUS_NOT_SUPPORTED;
  }

  uint32_t entry_address = 0;
  X_STATUS failure_status = X_STATUS_UNSUCCESSFUL;
  bool staged = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_states_.contains(thread->thread_id())) {
      failure_status = X_STATUS_NOT_SUPPORTED;
    } else {
      UserModeContextSnapshotResult snapshot = ResolveUserModeContextSnapshot(
          *kernel_state_->memory(), context_virtual_address);
      if (snapshot && WriteUserModeIntegerContextSnapshot(
                          *kernel_state_->memory(), context_virtual_address,
                          snapshot.context)) {
        const UserModeControlSelection selection =
            DecodeUserModeControlSelection(snapshot.context.user_mode_control);
        UserModeEnterBySelectorRequest request;
        request.thread_id = thread->thread_id();
        request.mapping_selector = selection.mapping_selector;
        request.context_virtual_address = context_virtual_address;
        request.entry_context = snapshot.context;
        request.handler_address = handler_address;
        request.save_area_end = save_area_end;
        request.companion_state = companion_state;
        request.outer_state = CaptureUserModeOuterIntegerState(*ppc_context);

        UserModeBeginEnterBySelectorResult begin =
            manager_->BeginEnterBySelector(request);
        if (!begin) {
          failure_status =
              begin.begin.guest_status.value_or(X_STATUS_UNSUCCESSFUL);
        } else {
          const UserModeMappingHandle mapping = begin.resolved_mapping->handle;
          std::shared_ptr<const UserModeGuestCodeDomainLease> lease =
              coordinator_->FindLease(mapping);
          const uint32_t exception_record =
              kernel_state_->memory()->SystemHeapAlloc(kHandlerBlockSize);
          if (exception_record) {
            std::memset(
                kernel_state_->memory()->TranslateVirtual(exception_record), 0,
                kHandlerBlockSize);
          }
          if (!lease || !exception_record) {
            manager_->Leave(thread->thread_id());
            if (exception_record) {
              kernel_state_->memory()->SystemHeapFree(exception_record);
            }
          } else {
            const UserModePreparedEntry& prepared_entry =
                *begin.begin.prepared_entry;
            ThreadRuntimeState runtime_state;
            runtime_state.mapping = mapping;
            runtime_state.thread_state = thread->thread_state();
            runtime_state.pcr_address = thread->pcr_ptr();
            runtime_state.kthread_address = thread->guest_object();
            runtime_state.context_virtual_address = context_virtual_address;
            runtime_state.exception_record_virtual_address = exception_record;
            bool inserted = false;
            try {
              inserted =
                  thread_states_.emplace(thread->thread_id(), runtime_state)
                      .second;
            } catch (...) {
              inserted = false;
            }
            if (inserted) {
              TrackThreadLocked(thread->thread_id());
            }
            const cpu::TranslationDomainKey target_domain =
                SelectDomain(*lease, prepared_entry.context.user_mode_control);
            if (inserted && prepared_entry.context.entry_address != 0 &&
                StageTransition(*thread, mapping,
                                cpu::TranslationDomainKey::DefaultDirect(),
                                target_domain)) {
              ApplyUserModeIntegerContext(prepared_entry.context, *ppc_context);
              XELOGD("KeEnterUserMode: entry {:08X}, context r1 {:016X}",
                     prepared_entry.context.entry_address,
                     snapshot.context.gpr[1]);
              ApplyRetailUserModeMachineState(
                  prepared_entry.context.user_mode_control, *ppc_context);
              entry_address = prepared_entry.context.entry_address;
              staged = true;
              PublishTrapFrameLocked(thread_states_.at(thread->thread_id()),
                                     true);
            }
            if (!staged) {
              if (inserted) {
                thread_states_.erase(thread->thread_id());
                UntrackThreadLocked(thread->thread_id());
              }
              manager_->Leave(thread->thread_id());
              kernel_state_->memory()->SystemHeapFree(exception_record);
            }
          }
        }
      }
    }
  }

  if (!staged) {
    return failure_status;
  }
  thread->ReenterPreparedFromPlainFrames(entry_address);
  std::abort();
}

bool UserModeKernelRuntime::DispatchGuestSyscall(
    cpu::ppc::PPCContext* ppc_context,
    const cpu::ppc::GuestSystemCallEvent& event) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context) {
    return false;
  }
  const cpu::TranslationDomainKey source_domain =
      thread->thread_state()->translation_domain_key();
  if (!IsCheckedDomain(source_domain)) {
    return false;
  }
  // 0 ends ExecuteGuestWithReentry, so reject a next CIA that wrapped to 0
  // before anything is published.
  if (!event.next_cia) {
    FailInvariant("zero problem-state resume address", ppc_context);
  }

  uint32_t reentry_address = 0;
  bool staged = false;
  bool invariant_failure = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread->thread_id());
    std::optional<UserModeActiveFrameSnapshot> active =
        manager_->FindActiveFrame(thread->thread_id());
    if (state_it == thread_states_.end() || !active ||
        active->prepared_entry.mapping != state_it->second.mapping ||
        state_it->second.pending_action != UserModePendingKernelAction::kNone) {
      invariant_failure = true;
    } else if (event.number <= 1) {
      UserModeProblemStateSystemCallResult syscall =
          manager_->DispatchProblemStateSystemCall(thread->thread_id(),
                                                   event.number);
      std::shared_ptr<const UserModeGuestCodeDomainLease> lease =
          coordinator_->FindLease(state_it->second.mapping);
      if (!syscall ||
          syscall.action !=
              UserModeProblemStateSystemCallAction::kSwitchSlbHalf ||
          !lease || syscall.mapping != state_it->second.mapping) {
        invariant_failure = true;
      } else {
        const cpu::TranslationDomainKey target_domain =
            SelectDomain(*lease, syscall.current_user_mode_control);
        staged = StageTransition(*thread, state_it->second.mapping,
                                 source_domain, target_domain);
        if (staged) {
          ppc_context->r[0] = 0;
          ApplyRetailUserModeMachineState(syscall.current_user_mode_control,
                                          *ppc_context);
          reentry_address = event.next_cia;
        } else {
          invariant_failure = true;
        }
      }
    } else {
      XELOGD(
          "XeFU user-mode sc r0={:X} at {:08X} delivered to handler (r3 {:08X} "
          "r4 {:08X} r11 {:08X} r12 {:08X} r13 {:08X} x86 eip {:08X})",
          event.number, event.cia, static_cast<uint32_t>(ppc_context->r[3]),
          static_cast<uint32_t>(ppc_context->r[4]),
          static_cast<uint32_t>(ppc_context->r[11]),
          static_cast<uint32_t>(ppc_context->r[12]),
          static_cast<uint32_t>(ppc_context->r[13]),
          static_cast<uint32_t>(ppc_context->r[2]));
      ApplyTrapFrameMarksLocked(state_it->second, *ppc_context);
      UserModeIntegerContext saved = active->prepared_entry.context;
      SaveUserModeIntegerContext(*ppc_context, saved);
      saved.entry_address = event.next_cia;
      saved.user_mode_control = active->current_user_mode_control;
      if (!WriteUserModeIntegerContextSnapshot(
              *kernel_state_->memory(),
              state_it->second.context_virtual_address, saved) ||
          !WriteHandlerTrapFrame(
              *kernel_state_->memory(),
              state_it->second.exception_record_virtual_address, saved)) {
        invariant_failure = true;
      } else {
        state_it->second.trap_frame_condition_register =
            saved.condition_register;
        UserModeProblemStateSystemCallResult syscall =
            manager_->DispatchProblemStateSystemCall(thread->thread_id(),
                                                     event.number);
        if (!syscall ||
            syscall.action !=
                UserModeProblemStateSystemCallAction::kDeliverToHandler ||
            syscall.mapping != state_it->second.mapping) {
          invariant_failure = true;
        } else if (!StageTransition(
                       *thread, state_it->second.mapping, source_domain,
                       cpu::TranslationDomainKey::DefaultDirect())) {
          invariant_failure = true;
        } else {
          state_it->second.pending_action =
              UserModePendingKernelAction::kHandler;
          state_it->second.pending_frame_revision = syscall.frame_revision;
          state_it->second.pending_resume_address = event.next_cia;
          ApplyUserModeHandlerEntryState(active->outer_state, saved,
                                         *ppc_context);
          ppc_context->r[3] = 0;
          ppc_context->r[4] = HandlerTrapFrameAddress(
              state_it->second.exception_record_virtual_address);
          ppc_context->ctr = syscall.handler_address;
          ppc_context->lr = handler_return_address_;
          reentry_address = syscall.handler_address;
          staged = reentry_address != 0;
        }
      }
    }
  }

  if (invariant_failure || !staged || !reentry_address) {
    FailInvariant("problem-state system call", ppc_context);
  }
  thread->ReenterPreparedFromPlainFrames(reentry_address);
  std::abort();
}

[[noreturn]] void UserModeKernelRuntime::HandleGuestMemoryFault(
    cpu::ppc::PPCContext* ppc_context,
    cpu::TranslationDomainKey translation_domain_key,
    const cpu::GuestMemoryAccessRequest& request,
    const cpu::GuestMemoryAccessResult& result) {
  XThread* thread = XThread::GetCurrentThread();
  if (!thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context ||
      thread->thread_state()->translation_domain_key() !=
          translation_domain_key) {
    FailInvariant("checked-memory fault ownership", ppc_context);
  }

  if (result.diagnostic == cpu::GuestMemoryAccessDiagnostic::kCommitRace ||
      result.fault.reason == cpu::GuestMemoryAccessFaultReason::kCommitRace) {
    // Validate the retry target before marking XThread reentry as staged.
    if (!request.instruction_address) {
      FailInvariant("zero checked-memory retry address", ppc_context);
    }
    bool staged = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      staged = thread->StageTranslationDomainReentry(translation_domain_key,
                                                     nullptr);
    }
    if (!staged) {
      FailInvariant("checked-memory translation retry", ppc_context);
    }
    thread->ReenterPreparedFromPlainFrames(request.instruction_address);
  }

  const bool guest_fault =
      result.diagnostic == cpu::GuestMemoryAccessDiagnostic::kGuestFault &&
      (result.fault.reason == cpu::GuestMemoryAccessFaultReason::kAlignment ||
       result.fault.reason ==
           cpu::GuestMemoryAccessFaultReason::kAddressTranslation ||
       result.fault.reason == cpu::GuestMemoryAccessFaultReason::kProtection);
  const bool backing_fault =
      result.diagnostic ==
          cpu::GuestMemoryAccessDiagnostic::kBackingUnavailable &&
      result.fault.reason ==
          cpu::GuestMemoryAccessFaultReason::kPhysicalBacking;
  // A bytewise (misaligned) access reports the byte that faulted.
  const bool fault_identity_matches =
      result.fault.instruction_address == request.instruction_address &&
      result.fault.effective_address - request.effective_address <
          std::max<uint32_t>(request.byte_count, 1) &&
      result.fault.byte_count == request.byte_count;
  const bool deliverable =
      fault_identity_matches && (guest_fault || backing_fault);
  XELOGD(
      "XeFU user-mode memory fault at {:08X}: EA {:08X}, {} bytes, reason {}, "
      "diagnostic {} (x86 eip {:08X})",
      request.instruction_address, request.effective_address,
      request.byte_count, static_cast<uint32_t>(result.fault.reason),
      static_cast<uint32_t>(result.diagnostic),
      static_cast<uint32_t>(ppc_context->r[2]));
  if (!deliverable) {
    FailInvariant("non-deliverable checked-memory fault", ppc_context);
  }

  DeliverExceptionToHandler(*thread, ppc_context, translation_domain_key,
                            request.instruction_address,
                            BuildUserModeMemoryExceptionRecord(request, result),
                            "checked-memory handler delivery");
}

bool UserModeKernelRuntime::DispatchGuestInstructionStorageFault(
    cpu::ppc::PPCContext* ppc_context,
    cpu::TranslationDomainKey translation_domain_key,
    const cpu::ppc::GuestInstructionFetchFault& fault) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context ||
      thread->thread_state()->translation_domain_key() !=
          translation_domain_key ||
      !IsCheckedDomain(translation_domain_key) || !fault.is_guest_fault()) {
    return false;
  }
  // Faults at 0 are frequent; log only some.
  static std::atomic<uint32_t> delivered_count{0};
  const uint32_t count = ++delivered_count;
  if (count <= 32 || !(count % 1024)) {
    XELOGD(
        "XeFU user-mode instruction storage fault {} at {:08X} (reason {}, "
        "LR {:08X}, CTR {:08X}, x86 eip {:08X})",
        count, fault.instruction_address, static_cast<uint32_t>(fault.reason),
        static_cast<uint32_t>(ppc_context->lr),
        static_cast<uint32_t>(ppc_context->ctr),
        static_cast<uint32_t>(ppc_context->r[2]));
  }
  // The address may be 0: SRR0 is the branch target.
  DeliverExceptionToHandler(
      *thread, ppc_context, translation_domain_key, fault.instruction_address,
      BuildUserModeInstructionStorageExceptionRecord(fault),
      "instruction-storage handler delivery");
}

bool UserModeKernelRuntime::DispatchGuestProgramException(
    cpu::ppc::PPCContext* ppc_context,
    const cpu::ppc::GuestProgramExceptionEvent& event) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context) {
    return false;
  }
  const cpu::TranslationDomainKey source_domain =
      thread->thread_state()->translation_domain_key();
  if (!IsCheckedDomain(source_domain)) {
    return false;
  }
  if (!event.cia) {
    FailInvariant("zero program-exception address", ppc_context);
  }
  XELOGD("XeFU user-mode program exception {} at {:08X}",
         static_cast<uint32_t>(event.kind), event.cia);
  DeliverExceptionToHandler(
      *thread, ppc_context, source_domain, event.cia,
      BuildUserModeProgramExceptionRecord(event.cia, event.kind),
      "program-exception handler delivery");
}

[[noreturn]] void UserModeKernelRuntime::DeliverExceptionToHandler(
    XThread& thread, cpu::ppc::PPCContext* ppc_context,
    cpu::TranslationDomainKey translation_domain_key, uint32_t resume_address,
    const X_EXCEPTION_RECORD& exception_record, const char* operation) {
  uint32_t handler_address = 0;
  bool staged = false;
  bool invariant_failure = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread.thread_id());
    std::optional<UserModeActiveFrameSnapshot> active =
        manager_->FindActiveFrame(thread.thread_id());
    if (state_it == thread_states_.end() || !active ||
        active->prepared_entry.mapping != state_it->second.mapping ||
        state_it->second.pending_action != UserModePendingKernelAction::kNone ||
        !state_it->second.exception_record_virtual_address) {
      invariant_failure = true;
    } else {
      ApplyTrapFrameMarksLocked(state_it->second, *ppc_context);
      UserModeIntegerContext saved = active->prepared_entry.context;
      SaveUserModeIntegerContext(*ppc_context, saved);
      saved.entry_address = resume_address;
      saved.user_mode_control = active->current_user_mode_control;
      if (!WriteUserModeIntegerContextSnapshot(
              *kernel_state_->memory(),
              state_it->second.context_virtual_address, saved) ||
          !WriteHandlerTrapFrame(
              *kernel_state_->memory(),
              state_it->second.exception_record_virtual_address, saved)) {
        invariant_failure = true;
      } else {
        state_it->second.trap_frame_condition_register =
            saved.condition_register;
        UserModeHandlerDeliveryResult delivery =
            manager_->PrepareHandlerDelivery(thread.thread_id());
        if (!delivery || delivery.mapping != state_it->second.mapping) {
          invariant_failure = true;
        } else {
          std::memcpy(kernel_state_->memory()->TranslateVirtual(
                          state_it->second.exception_record_virtual_address),
                      &exception_record, sizeof(exception_record));
          if (!StageTransition(thread, state_it->second.mapping,
                               translation_domain_key,
                               cpu::TranslationDomainKey::DefaultDirect())) {
            invariant_failure = true;
          } else {
            state_it->second.pending_action =
                UserModePendingKernelAction::kHandler;
            state_it->second.pending_frame_revision = delivery.frame_revision;
            state_it->second.pending_resume_address = resume_address;
            ApplyUserModeHandlerEntryState(active->outer_state, saved,
                                           *ppc_context);
            ppc_context->r[3] =
                state_it->second.exception_record_virtual_address;
            ppc_context->r[4] = HandlerTrapFrameAddress(
                state_it->second.exception_record_virtual_address);
            ppc_context->ctr = delivery.handler_address;
            ppc_context->lr = handler_return_address_;
            handler_address = delivery.handler_address;
            staged = handler_address != 0;
          }
        }
      }
    }
  }

  if (invariant_failure || !staged || !handler_address) {
    FailInvariant(operation, ppc_context);
  }
  thread.ReenterPreparedFromPlainFrames(handler_address);
}

void UserModeKernelRuntime::Flush(uint32_t selector,
                                  cpu::ppc::PPCContext* ppc_context) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context) {
    return;
  }

  const cpu::TranslationDomainKey source_domain =
      thread->thread_state()->translation_domain_key();
  if (source_domain == cpu::TranslationDomainKey::DefaultDirect()) {
    std::optional<UserModeMappingSnapshot> mapping =
        manager_->FindMappingBySelector(selector);
    if (mapping) {
      // Live: another thread may be running this mapping's code.
      const cpu::GuestCodeInvalidationResult flushed =
          coordinator_->InvalidateMappingDomainsLive(mapping->handle);
      if (!flushed) {
        XELOGE("KeFlushUserModeTb failed for selector {} (status {})", selector,
               static_cast<uint32_t>(flushed.status));
      }
    }
    return;
  }

  uint32_t continuation_address = 0;
  bool staged = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread->thread_id());
    std::optional<UserModeMappingSnapshot> flush_mapping =
        manager_->FindMappingBySelector(selector);
    const uint32_t resume_address = static_cast<uint32_t>(ppc_context->lr);
    if (state_it != thread_states_.end() && resume_address != 0 &&
        flush_mapping &&
        state_it->second.pending_action == UserModePendingKernelAction::kNone) {
      staged = StageTransition(*thread, state_it->second.mapping, source_domain,
                               cpu::TranslationDomainKey::DefaultDirect());
      if (staged) {
        state_it->second.pending_action = UserModePendingKernelAction::kFlush;
        state_it->second.pending_flush_mapping = flush_mapping->handle;
        state_it->second.pending_resume_address = resume_address;
        continuation_address = continuation_function_->address();
      }
    }
  }
  if (!staged || !continuation_address) {
    FailInvariant("checked-domain TLB flush", ppc_context);
  }
  thread->ReenterPreparedFromPlainFrames(continuation_address);
}

bool UserModeKernelRuntime::QueueThreadDpc(uint32_t thread_id, uint32_t dpc_ptr,
                                           uint32_t arg1, uint32_t arg2) {
  if (!is_ready_ || !dpc_ptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto state_it = thread_states_.find(thread_id);
  if (state_it == thread_states_.end() || !state_it->second.thread_state) {
    return false;
  }
  QueuePendingDpc(state_it->second, dpc_ptr, arg1, arg2);
  return true;
}

bool UserModeKernelRuntime::QueueProcessorDpc(uint8_t cpu, uint32_t dpc_ptr,
                                              uint32_t arg1, uint32_t arg2) {
  if (!is_ready_ || !dpc_ptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [thread_id, state] : thread_states_) {
    // The PRCB is read live: KeSetAffinityThread may move the thread.
    if (state.thread_state && state.pcr_address &&
        kernel_state_->memory()
                ->TranslateVirtual<const X_KPCR*>(state.pcr_address)
                ->prcb_data.current_cpu == cpu) {
      QueuePendingDpc(state, dpc_ptr, arg1, arg2);
      return true;
    }
  }
  return false;
}

void UserModeKernelRuntime::ApplyTrapFrameMarksLocked(
    ThreadRuntimeState& state, cpu::ppc::PPCContext& ppc_context) {
  if (!state.exception_record_virtual_address) {
    return;
  }
  const uint32_t frame_cr = ReadTrapFrameConditionRegister(
      *kernel_state_->memory(), state.exception_record_virtual_address);
  ppc_context.set_cr(ApplyTrapFrameConditionRegisterMarks(
      static_cast<uint32_t>(ppc_context.cr()), frame_cr,
      state.trap_frame_condition_register));
  state.trap_frame_condition_register = frame_cr;
}

void UserModeKernelRuntime::PublishTrapFrameLocked(
    const ThreadRuntimeState& state, bool publish) {
  if (!state.kthread_address) {
    return;
  }
  auto* kthread = kernel_state_->memory()->TranslateVirtual<X_KTHREAD*>(
      state.kthread_address);
  kthread->user_mode_trap_frame =
      publish && state.exception_record_virtual_address
          ? HandlerTrapFrameAddress(state.exception_record_virtual_address)
          : 0;
}

void UserModeKernelRuntime::QueuePendingDpc(ThreadRuntimeState& state,
                                            uint32_t dpc_ptr, uint32_t arg1,
                                            uint32_t arg2) {
  std::vector<PendingDpc>& dpcs = state.pending_dpcs;
  // Like KeInsertQueueDpc, a DPC that is already queued is not queued again.
  if (std::none_of(dpcs.cbegin(), dpcs.cend(),
                   [dpc_ptr](const PendingDpc& dpc) {
                     return dpc.dpc_ptr == dpc_ptr;
                   })) {
    dpcs.push_back({dpc_ptr, arg1, arg2});
  }
  state.thread_state->set_guest_interrupt_pending(true);
}

void UserModeKernelRuntime::PollGuestInterrupt(
    cpu::ppc::PPCContext* ppc_context, uint32_t resume_address) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context || !resume_address) {
    return;
  }
  const cpu::TranslationDomainKey source_domain =
      thread->thread_state()->translation_domain_key();
  if (!IsCheckedDomain(source_domain)) {
    return;
  }
  uint32_t continuation_address = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread->thread_id());
    if (state_it == thread_states_.end()) {
      thread->thread_state()->set_guest_interrupt_pending(false);
      return;
    }
    ThreadRuntimeState& state = state_it->second;
    if (state.pending_dpcs.empty()) {
      thread->thread_state()->set_guest_interrupt_pending(false);
      return;
    }
    std::optional<UserModeActiveFrameSnapshot> active =
        manager_->FindActiveFrame(thread->thread_id());
    if (state.pending_action != UserModePendingKernelAction::kNone || !active ||
        !StageTransition(*thread, state.mapping, source_domain,
                         cpu::TranslationDomainKey::DefaultDirect())) {
      // Take the branch; the next one polls again.
      return;
    }
    const auto* context_bytes = reinterpret_cast<const uint8_t*>(ppc_context);
    state.interrupted_context.assign(context_bytes,
                                     context_bytes + sizeof(*ppc_context));
    state.pending_action = UserModePendingKernelAction::kInterrupt;
    state.pending_resume_address = resume_address;
    // DPCs run on the kernel stack with the kernel PCR, as after an interrupt
    // taken in user mode.
    ApplyUserModeOuterIntegerState(active->outer_state, *ppc_context);
    continuation_address = continuation_function_->address();
  }
  thread->ReenterPreparedFromPlainFrames(continuation_address);
}

[[noreturn]] void UserModeKernelRuntime::ReturnFromInterrupt(
    XThread& thread, cpu::ppc::PPCContext* ppc_context) {
  uint32_t trap_frame = 0;
  uint32_t written_condition_register = 0;
  // The pending DPCs, copied out under the lock. Nothing owning may be alive
  // at the reentry below, so the overflow vector lives in this scope.
  constexpr size_t kDpcBatch = 16;
  std::array<PendingDpc, kDpcBatch> dpc_batch;
  size_t dpc_count = 0;
  {
    std::vector<PendingDpc> dpc_overflow;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto state_it = thread_states_.find(thread.thread_id());
      if (state_it != thread_states_.end()) {
        ThreadRuntimeState& state = state_it->second;
        for (const PendingDpc& dpc : state.pending_dpcs) {
          if (dpc_count < kDpcBatch) {
            dpc_batch[dpc_count++] = dpc;
          } else {
            dpc_overflow.push_back(dpc);
          }
        }
        state.pending_dpcs.clear();
        thread.thread_state()->set_guest_interrupt_pending(false);
        // The interrupted context goes into the thread's trap frame while its
        // DPCs run, as for an interrupt taken in user mode; DPCs may mark it.
        if (state.pending_action == UserModePendingKernelAction::kInterrupt &&
            state.interrupted_context.size() == sizeof(*ppc_context)) {
          cpu::ppc::PPCContext interrupted;
          std::memcpy(&interrupted, state.interrupted_context.data(),
                      sizeof(*ppc_context));
          ApplyTrapFrameMarksLocked(state, interrupted);
          UserModeIntegerContext frame_context{};
          SaveUserModeIntegerContext(interrupted, frame_context);
          frame_context.entry_address = state.pending_resume_address;
          if (WriteHandlerTrapFrame(*kernel_state_->memory(),
                                    state.exception_record_virtual_address,
                                    frame_context)) {
            trap_frame =
                HandlerTrapFrameAddress(state.exception_record_virtual_address);
            state.trap_frame_condition_register =
                frame_context.condition_register;
            written_condition_register = frame_context.condition_register;
          }
        }
      }
    }
    // DPCs queued meanwhile (a DPC re-arming its timer) wait for the next
    // interrupt.
    for (size_t i = 0; i < dpc_count; ++i) {
      xboxkrnl::xeKeExecuteDpc(dpc_batch[i].dpc_ptr, dpc_batch[i].arg1,
                               dpc_batch[i].arg2);
    }
    for (const PendingDpc& dpc : dpc_overflow) {
      xboxkrnl::xeKeExecuteDpc(dpc.dpc_ptr, dpc.arg1, dpc.arg2);
    }
  }
  // Only the CR1 marks are taken back from the frame; its 32-bit LR field
  // would truncate LR.
  std::optional<uint32_t> frame_condition_register;
  if (trap_frame) {
    UserModeIntegerContext frame_context{};
    const auto* frame =
        kernel_state_->memory()->TranslateVirtual<const uint8_t*>(trap_frame);
    if (DecodeUserModeTrapFrame(
            std::span<const uint8_t>(frame, kUserModeTrapFrameSize),
            frame_context) == UserModeContextCodecError::kNone) {
      frame_condition_register = frame_context.condition_register;
    }
  }

  uint32_t reentry_address = 0;
  bool staged = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread.thread_id());
    std::optional<UserModeActiveFrameSnapshot> active =
        manager_->FindActiveFrame(thread.thread_id());
    if (state_it != thread_states_.end() && active &&
        state_it->second.pending_action ==
            UserModePendingKernelAction::kInterrupt &&
        state_it->second.interrupted_context.size() == sizeof(*ppc_context)) {
      ThreadRuntimeState& state = state_it->second;
      std::shared_ptr<const UserModeGuestCodeDomainLease> lease =
          coordinator_->FindLease(state.mapping);
      if (lease &&
          StageTransition(
              thread, state.mapping, cpu::TranslationDomainKey::DefaultDirect(),
              SelectDomain(*lease, active->current_user_mode_control))) {
        std::memcpy(ppc_context, state.interrupted_context.data(),
                    sizeof(*ppc_context));
        if (frame_condition_register) {
          ppc_context->set_cr(ApplyTrapFrameConditionRegisterMarks(
              written_condition_register, *frame_condition_register,
              written_condition_register));
          state.trap_frame_condition_register = *frame_condition_register;
        }
        reentry_address = state.pending_resume_address;
        staged = reentry_address != 0;
      }
      state.interrupted_context.clear();
      state.pending_action = UserModePendingKernelAction::kNone;
      state.pending_resume_address = 0;
      // The restored context carries the stale pending byte; resync it.
      thread.thread_state()->set_guest_interrupt_pending(
          !state.pending_dpcs.empty());
    }
  }
  if (!staged) {
    FailInvariant("user-mode interrupt return", ppc_context);
  }
  thread.ReenterPreparedFromPlainFrames(reentry_address);
}

void UserModeKernelRuntime::FlushEntireTb(cpu::ppc::PPCContext* ppc_context) {
  XThread* thread = XThread::GetCurrentThread();
  if (!is_ready_ || !thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context ||
      thread->thread_state()->translation_domain_key() !=
          cpu::TranslationDomainKey::DefaultDirect()) {
    return;
  }
  const cpu::GuestCodeInvalidationResult flushed =
      coordinator_->InvalidateAllMappingDomainsLive();
  if (!flushed) {
    XELOGE("KeFlushCurrentEntireTb failed (status {})",
           static_cast<uint32_t>(flushed.status));
  }
}

void UserModeKernelRuntime::ContinuationThunk(cpu::ppc::PPCContext* ppc_context,
                                              void* arg0, void* arg1) {
  (void)arg1;
  static_cast<UserModeKernelRuntime*>(arg0)->Continue(ppc_context);
}

[[noreturn]] void UserModeKernelRuntime::Continue(
    cpu::ppc::PPCContext* ppc_context) {
  XThread* thread = XThread::GetCurrentThread();
  if (!thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context ||
      thread->thread_state()->translation_domain_key() !=
          cpu::TranslationDomainKey::DefaultDirect()) {
    FailInvariant("user-mode continuation ownership", ppc_context);
  }

  // A pending TB flush runs live and outside mutex_: it takes the resolution
  // lock, which is never acquired under the runtime lock.
  std::optional<UserModeMappingHandle> flush_mapping;
  bool interrupt = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread->thread_id());
    if (state_it != thread_states_.end()) {
      if (state_it->second.pending_action ==
          UserModePendingKernelAction::kFlush) {
        flush_mapping = state_it->second.pending_flush_mapping;
      }
      interrupt = state_it->second.pending_action ==
                  UserModePendingKernelAction::kInterrupt;
    }
  }
  if (interrupt) {
    ReturnFromInterrupt(*thread, ppc_context);
  }
  if (flush_mapping) {
    const cpu::GuestCodeInvalidationResult flushed =
        coordinator_->InvalidateMappingDomainsLive(*flush_mapping);
    if (!flushed) {
      XELOGE("KeFlushUserModeTb from user mode failed (status {})",
             static_cast<uint32_t>(flushed.status));
    }
  }

  uint32_t reentry_address = 0;
  bool staged = false;
  bool invariant_failure = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = thread_states_.find(thread->thread_id());
    if (state_it == thread_states_.end()) {
      invariant_failure = true;
    } else if (state_it->second.pending_action ==
               UserModePendingKernelAction::kFlush) {
      std::optional<UserModeActiveFrameSnapshot> active =
          manager_->FindActiveFrame(thread->thread_id());
      std::shared_ptr<const UserModeGuestCodeDomainLease> lease =
          coordinator_->FindLease(state_it->second.mapping);
      if (!active || !lease) {
        invariant_failure = true;
      } else {
        const cpu::TranslationDomainKey target_domain =
            SelectDomain(*lease, active->current_user_mode_control);
        staged = StageTransition(*thread, state_it->second.mapping,
                                 cpu::TranslationDomainKey::DefaultDirect(),
                                 target_domain);
        if (staged) {
          reentry_address = state_it->second.pending_resume_address;
          state_it->second.pending_action = UserModePendingKernelAction::kNone;
          state_it->second.pending_flush_mapping = {};
          state_it->second.pending_resume_address = 0;
        } else {
          invariant_failure = true;
        }
      }
    } else if (state_it->second.pending_action ==
               UserModePendingKernelAction::kHandler) {
      UserModeContextSnapshotResult snapshot = ResolveUserModeContextSnapshot(
          *kernel_state_->memory(), state_it->second.context_virtual_address);
      // The handler edits the interrupted state in the trap frame; resume
      // from it. UserModeControl has no trap-frame field.
      UserModeIntegerContext resume_context;
      bool frame_decoded = false;
      if (snapshot) {
        resume_context = snapshot.context;
        const auto* frame = kernel_state_->memory()->TranslateVirtual<uint8_t*>(
            HandlerTrapFrameAddress(
                state_it->second.exception_record_virtual_address));
        frame_decoded =
            DecodeUserModeTrapFrame(
                std::span<const uint8_t>(frame, kUserModeTrapFrameSize),
                resume_context) == UserModeContextCodecError::kNone;
      }
      if (!snapshot || !frame_decoded) {
        invariant_failure = true;
      } else {
        const UserModeControlSelection selection =
            DecodeUserModeControlSelection(snapshot.context.user_mode_control);
        UserModeHandlerResumeRequest resume;
        resume.thread_id = thread->thread_id();
        resume.mapping = state_it->second.mapping;
        resume.expected_frame_revision =
            state_it->second.pending_frame_revision;
        resume.user_mode_control = snapshot.context.user_mode_control;
        // The snapshot keeps the faulting IAR, 0 after an instruction storage
        // fault at 0; only the IAR the handler leaves in the frame counts.
        const std::optional<uint32_t> resume_address =
            SelectUserModeHandlerResumeAddress(resume_context);
        if (!resume_address) {
          XELOGE(
              "XeFU handler resumed at IAR 0; the console would fault there "
              "again");
          invariant_failure = true;
        } else {
          UserModeHandlerResumeResult resumed = manager_->ResumeHandler(resume);
          std::shared_ptr<const UserModeGuestCodeDomainLease> lease =
              coordinator_->FindLease(state_it->second.mapping);
          if (!resumed || !lease ||
              selection.mapping_selector != state_it->second.mapping.selector) {
            invariant_failure = true;
          } else {
            const cpu::TranslationDomainKey target_domain =
                SelectDomain(*lease, snapshot.context.user_mode_control);
            staged = StageTransition(*thread, state_it->second.mapping,
                                     cpu::TranslationDomainKey::DefaultDirect(),
                                     target_domain);
            if (staged) {
              ApplyUserModeIntegerContext(resume_context, *ppc_context);
              ApplyRetailUserModeMachineState(
                  snapshot.context.user_mode_control, *ppc_context);
              state_it->second.trap_frame_condition_register =
                  resume_context.condition_register;
              reentry_address = resume_context.entry_address;
              state_it->second.pending_action =
                  UserModePendingKernelAction::kNone;
              state_it->second.pending_frame_revision = 0;
              state_it->second.pending_resume_address = 0;
            } else {
              invariant_failure = true;
            }
          }
        }
      }
    } else {
      invariant_failure = true;
    }
  }

  if (invariant_failure || !staged || !reentry_address) {
    FailInvariant("user-mode continuation", ppc_context);
  }
  XELOGD("XeFU user-mode continuation resumes at {:08X} (r3 {:08X} r11 {:08X})",
         reentry_address, static_cast<uint32_t>(ppc_context->r[3]),
         static_cast<uint32_t>(ppc_context->r[11]));
  thread->ReenterPreparedFromPlainFrames(reentry_address);
}

bool UserModeKernelRuntime::ClearThreadRuntimeStateLocked(
    uint32_t thread_id, uint32_t* exception_record_out,
    std::vector<PendingDpc>* dpcs_out) {
  auto state_it = thread_states_.find(thread_id);
  if (state_it == thread_states_.end()) {
    return false;
  }
  if (exception_record_out) {
    *exception_record_out = state_it->second.exception_record_virtual_address;
  }
  if (state_it->second.thread_state) {
    state_it->second.thread_state->set_guest_interrupt_pending(false);
  }
  if (dpcs_out) {
    *dpcs_out = std::move(state_it->second.pending_dpcs);
  }
  PublishTrapFrameLocked(state_it->second, false);
  UntrackThreadLocked(state_it->first);
  thread_states_.erase(state_it);
  return true;
}

void UserModeKernelRuntime::TrackThreadLocked(uint32_t thread_id) {
  for (auto& slot : tracked_threads_) {
    if (!slot.load(std::memory_order_relaxed)) {
      slot.store(thread_id, std::memory_order_release);
      return;
    }
  }
  tracked_threads_overflow_.store(true, std::memory_order_release);
}

void UserModeKernelRuntime::UntrackThreadLocked(uint32_t thread_id) {
  for (auto& slot : tracked_threads_) {
    if (slot.load(std::memory_order_relaxed) == thread_id) {
      slot.store(0, std::memory_order_release);
    }
  }
}

bool UserModeKernelRuntime::MayHaveThreadState(uint32_t thread_id) const {
  if (tracked_threads_overflow_.load(std::memory_order_acquire)) {
    return true;
  }
  for (const auto& slot : tracked_threads_) {
    if (slot.load(std::memory_order_acquire) == thread_id) {
      return true;
    }
  }
  return false;
}

void UserModeKernelRuntime::Leave(cpu::ppc::PPCContext* ppc_context) {
  XThread* thread = XThread::GetCurrentThread();
  if (!thread || !ppc_context ||
      thread->thread_state()->context() != ppc_context) {
    FailInvariant("KeLeaveUserMode ownership", ppc_context);
  }

  bool success = false;
  bool staged = false;
  uint32_t exception_record = 0;
  uint32_t resume_address = 0;
  // Nothing owning may be alive at the reentry below.
  {
    std::vector<PendingDpc> dpcs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (thread->thread_state()->translation_domain_key() ==
          cpu::TranslationDomainKey::DefaultDirect()) {
        UserModeLeaveResult leave = manager_->Leave(thread->thread_id());
        if (leave) {
          ApplyRetailUserModeLeaveState(*leave.outer_state, *ppc_context);
          success = ClearThreadRuntimeStateLocked(thread->thread_id(),
                                                  &exception_record, &dpcs);
          // Leave returns to KeEnterUserMode's caller at the restored LR,
          // through a reentry: Enter already discarded that caller's host
          // frames.
          resume_address = static_cast<uint32_t>(ppc_context->lr);
          staged = success && resume_address &&
                   thread->StageTranslationDomainReentry(
                       cpu::TranslationDomainKey::DefaultDirect(), nullptr);
        }
      }
    }
    if (exception_record) {
      kernel_state_->memory()->SystemHeapFree(exception_record);
    }
    // DPCs that were waiting for user mode run on the timer DPC thread now.
    for (const PendingDpc& dpc : dpcs) {
      kernel_state_->QueueTimerDpc(dpc.dpc_ptr, dpc.arg1, dpc.arg2);
    }
  }
  if (!success) {
    FailInvariant("KeLeaveUserMode without an active handler frame",
                  ppc_context);
  }
  if (!staged) {
    FailInvariant("KeLeaveUserMode resume", ppc_context);
  }
  XELOGD("XeFU KeLeaveUserMode resumes at {:08X} with r3={:08X}",
         resume_address, static_cast<uint32_t>(ppc_context->r[3]));
  thread->ReenterPreparedFromPlainFrames(resume_address);
}

void UserModeKernelRuntime::OnThreadDestroyed(uint32_t thread_id) {
  uint32_t exception_record = 0;
  if (MayHaveThreadState(thread_id)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearThreadRuntimeStateLocked(thread_id, &exception_record);
  }
  manager_->OnThreadDestroyed(thread_id);
  if (exception_record) {
    kernel_state_->memory()->SystemHeapFree(exception_record);
  }
}

void UserModeKernelRuntime::ResetTitle() {
  // The link's receive DPC must not outlive the title's threads.
  xefu_link::Shutdown();
  std::vector<uint32_t> exception_records;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exception_records.reserve(thread_states_.size());
    for (const auto& [thread_id, state] : thread_states_) {
      (void)thread_id;
      PublishTrapFrameLocked(state, false);
      if (state.exception_record_virtual_address) {
        exception_records.push_back(state.exception_record_virtual_address);
      }
    }
    thread_states_.clear();
    for (auto& slot : tracked_threads_) {
      slot.store(0, std::memory_order_release);
    }
    tracked_threads_overflow_.store(false, std::memory_order_release);
  }
  manager_->ResetTitle();
  for (uint32_t exception_record : exception_records) {
    kernel_state_->memory()->SystemHeapFree(exception_record);
  }
}

size_t UserModeKernelRuntime::active_thread_state_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return thread_states_.size();
}

[[noreturn]] void UserModeKernelRuntime::FailInvariant(
    const char* operation, cpu::ppc::PPCContext* ppc_context) {
  XELOGE("Fatal XeFU user-mode runtime invariant failure: {}", operation);
  XThread* thread = XThread::GetCurrentThread();
  if (thread &&
      (!ppc_context || thread->thread_state()->context() == ppc_context)) {
    thread->Terminate(static_cast<int>(X_STATUS_UNSUCCESSFUL));
  }
  std::abort();
}

}  // namespace xe::kernel

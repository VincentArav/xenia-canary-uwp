/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_seh.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_mode_context.h"
#include "xenia/kernel/util/ppc_unwind.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_bool(guest_exception_dispatch, false,
            "Dispatch exceptions raised with RtlRaiseException to the title's "
            "own structured exception handlers (__try/__except) instead of "
            "logging and returning. Always on for the XNA Indie Games player.",
            "Kernel");

// Guest structured exception handling.
//
// RtlRaiseException captures its caller's registers and walks the guest
// frames with the images' .pdata, calling each language handler as a guest
// function: handler(ExceptionRecord, EstablisherFrame, ContextRecord,
// DispatcherContext). __C_specific_handler runs host-side; it calls filters
// and termination handlers as guest funclets with r12 = EstablisherFrame (the
// function's entry r1), and an accepting filter makes it unwind to the
// __except block. RtlUnwind calls the handlers of the frames it removes with
// EXCEPTION_UNWINDING (EXCEPTION_TARGET_UNWIND for the target frame) and then
// transfers to the target with the target frame's registers, IAR = TargetIp
// and r3 = ReturnValue.
//
// Processor::Execute enters guest routines with LR = 0xBCBCBCBC. A walk that
// reaches such a frame continues from the registers recorded for that call
// (where the exception was raised or the unwind started).
//
// A target among the guest frames of the kernel call that started the work is
// reached by returning from that call with the target's registers and
// LR = TargetIp:
//  - a guest RtlUnwind whose target is among its caller's frames applies the
//    registers and returns;
//  - __C_specific_handler unwinding to an __except block of the exception it
//    handles hands the target to the dispatcher, and RtlRaiseException returns
//    into the __except block.
// Any other target lies beyond host frames. XThread::Reenter abandons them and
// resumes the target from the thread's dispatch loop, which is possible only
// from the outermost guest dispatch; under a nested host call (a DPC, APC or
// callback) the thread is stopped instead. Collided unwinds stop it too.
// Exceptions raised in epilogues and vector registers are not handled.
//
// Only RtlRaiseException's dispatch is gated (see DispatchGuestException). An
// RtlUnwind whose target no walk reaches returns without running any handler.

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Defined in xboxkrnl_debug.cc.
void KeBugCheckEx_entry(dword_t code, dword_t param1, dword_t param2,
                        dword_t param3, dword_t param4);

namespace {

using util::PpcRegisterState;

// The return address Processor::Execute calls guest code with.
constexpr uint32_t kHostCallReturnAddress = 0xBCBCBCBC;
// Processor::Execute lowers r1 by 64 + 112 bytes before the call, so the
// routine it enters runs with r1 exactly this far below the r1 the caller
// handed it.
constexpr uint32_t kHostCallStackPad = 64 + 112;
// __C_specific_handler looks up the call that entered it within this window,
// in case a guest wrapper's frame lies between.
constexpr uint32_t kHostCallStackWindow = 0x400;
constexpr uint32_t kMaxWalkFrames = 1024;
constexpr uint32_t kMaxGuestCalls = 32;
// Kept clear above the stack limit (the guard page) of the thread's stack.
constexpr uint32_t kStackLimitMargin = 0x400;
constexpr uint32_t kBugCheckKmodeExceptionNotHandled = 0x1E;

// Scratch on the guest stack below the caller's r1, where a kernel routine's
// frame would be. Guest routines called from here run below it.
constexpr uint32_t kStateStorageSize = 0x230;
static_assert(util::kPpcContextEncodedSize <= kStateStorageSize);
// Dispatch: the CONTEXT handlers see (and may edit), the registers at the
// raise (where a walk crossing the handler call continues) and the
// DISPATCHER_CONTEXT.
constexpr uint32_t kDispatchContextOffset = 0;
constexpr uint32_t kDispatchRaiseStateOffset = util::kPpcContextSize;
constexpr uint32_t kDispatchDispatcherContextOffset =
    kDispatchRaiseStateOffset + kStateStorageSize;
constexpr uint32_t kDispatchFrameSize =
    kDispatchDispatcherContextOffset + util::kDispatcherContextSize;
// Unwind: the CONTEXT of the frame being unwound, the registers the unwind
// started from, the DISPATCHER_CONTEXT and the EXCEPTION_RECORD used when
// the caller passes none.
constexpr uint32_t kUnwindContextOffset = 0;
constexpr uint32_t kUnwindStartStateOffset = util::kPpcContextSize;
constexpr uint32_t kUnwindDispatcherContextOffset =
    kUnwindStartStateOffset + kStateStorageSize;
constexpr uint32_t kUnwindRecordOffset =
    kUnwindDispatcherContextOffset + util::kDispatcherContextSize;
constexpr uint32_t kUnwindFrameSize =
    kUnwindRecordOffset + util::kExceptionRecordSize;
// __C_specific_handler: the EXCEPTION_POINTERS passed to filters.
constexpr uint32_t kCHandlerFrameSize = 0x10;

enum class SehFailure : uint32_t {
  kCollidedUnwind = 1,
  kInvalidDisposition = 2,
  kTargetLost = 3,
  kNestedHostCall = 4,
  kHandlerNotRun = 5,
};

const char* SehFailureText(SehFailure failure) {
  switch (failure) {
    case SehFailure::kCollidedUnwind:
      return "collided unwind (unsupported)";
    case SehFailure::kInvalidDisposition:
      return "invalid handler disposition during unwind";
    case SehFailure::kTargetLost:
      return "unwind target lost after handlers ran";
    case SehFailure::kNestedHostCall:
      return "unwind target lies under a nested host call";
    case SehFailure::kHandlerNotRun:
      return "an unwind handler could not run";
  }
  return "unknown failure";
}

// Unwinding has run guest handlers and cannot continue or go back: stop the
// thread rather than resume it in an inconsistent state.
void ReportSehFailure(SehFailure failure, uint32_t code, uint32_t target_ip,
                      uint32_t target_frame) {
  XELOGE(
      "Guest SEH: {} (exception {:08X}, target {:08X}, frame {:08X}); "
      "stopping the thread",
      SehFailureText(failure), code, target_ip, target_frame);
  KeBugCheckEx_entry(
      shim::ParamBase<uint32_t>(kBugCheckKmodeExceptionNotHandled),
      shim::ParamBase<uint32_t>(code), shim::ParamBase<uint32_t>(target_ip),
      shim::ParamBase<uint32_t>(target_frame),
      shim::ParamBase<uint32_t>(static_cast<uint32_t>(failure)));
}

// A guest routine (language handler, filter or termination handler) that
// host code here called and is waiting on. A walk that reaches the frame the
// routine was entered with continues from continuation_state.
struct GuestCall {
  uint32_t call_sp = 0;             // r1 handed to Processor::Execute
  uint32_t callee_depth = 0;        // guest dispatch depth inside the call
  uint32_t continuation_state = 0;  // guest CONTEXT copy; 0 ends the walk
  uint32_t continuation_depth = 0;  // dispatch depth of those frames
  // For calls made while dispatching: the frame being dispatched, up to which
  // a nested exception's walk marks frames EXCEPTION_NESTED_CALL.
  uint32_t nested_establisher = 0;
  bool unwind = false;  // made by an unwind: crossing it collides
  // The dispatcher's call of a language handler: an unwind the handler
  // requests may hand its target back to the dispatcher.
  bool language_handler = false;
};

// Calls in progress on this thread, outermost first. Plain data: a nonlocal
// transfer may abandon entries, which PruneAbandonedGuestCalls drops.
thread_local std::array<GuestCall, kMaxGuestCalls> t_guest_calls{};
thread_local uint32_t t_guest_call_count = 0;

// An unwind target handed to the dispatch whose raise state is at owner.
struct PendingTransfer {
  uint32_t owner = 0;  // 0: none
  PpcRegisterState state;
};
thread_local PendingTransfer t_pending_transfer{};

// The reentry landing resets the dispatch depth below calls it abandoned.
void PruneAbandonedGuestCalls(uint32_t current_depth) {
  while (t_guest_call_count &&
         t_guest_calls[t_guest_call_count - 1].callee_depth > current_depth) {
    --t_guest_call_count;
  }
}

// The innermost call whose routine was entered with r1 = entry_sp, or, unless
// exact, within kHostCallStackWindow of it. A walk crossing a host boundary
// needs the exact match: another host call may have been entered nearby.
const GuestCall* FindGuestCall(uint32_t entry_sp, bool exact) {
  for (uint32_t i = t_guest_call_count; i--;) {
    const GuestCall& call = t_guest_calls[i];
    if (entry_sp > call.call_sp) {
      continue;
    }
    const uint32_t distance = call.call_sp - entry_sp;
    if (exact ? distance == kHostCallStackPad
              : distance <= kHostCallStackWindow) {
      return &call;
    }
  }
  return nullptr;
}

class GuestReader final : public util::GuestMemoryReader {
 public:
  explicit GuestReader(Memory* memory) : memory_(memory) {}

  bool Read32(uint32_t address, uint32_t* value) const override {
    if (!IsAccessible(address, 4, false)) {
      return false;
    }
    *value = xe::load_and_swap<uint32_t>(
        memory_->TranslateVirtual<const uint8_t*>(address));
    return true;
  }

  bool Read64(uint32_t address, uint64_t* value) const override {
    if (!IsAccessible(address, 8, false)) {
      return false;
    }
    *value = xe::load_and_swap<uint64_t>(
        memory_->TranslateVirtual<const uint8_t*>(address));
    return true;
  }

  bool IsAccessible(uint32_t address, uint32_t size, bool write) const {
    if (!size || address > UINT32_MAX - (size - 1)) {
      return false;
    }
    BaseHeap* heap = memory_->LookupHeap(address);
    if (!heap) {
      return false;
    }
    const auto needed =
        static_cast<uint32_t>(write ? xe::memory::PageAccess::kReadWrite
                                    : xe::memory::PageAccess::kReadOnly);
    const auto access = static_cast<uint32_t>(
        heap->QueryRangeAccess(address, address + size - 1));
    return (access & needed) == needed;
  }

 private:
  Memory* memory_;
};

void WriteGuest32(Memory* memory, uint32_t address, uint32_t value) {
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(address), value);
}

void WriteContext(Memory* memory, uint32_t address,
                  const PpcRegisterState& state) {
  util::EncodePpcContext(state,
                         std::span<uint8_t>(memory->TranslateVirtual(address),
                                            util::kPpcContextEncodedSize));
}

bool ReadContext(const GuestReader& reader, Memory* memory, uint32_t address,
                 PpcRegisterState* state) {
  if (!reader.IsAccessible(address, util::kPpcContextEncodedSize, false)) {
    return false;
  }
  return util::DecodePpcContext(
      std::span<const uint8_t>(
          memory->TranslateVirtual<const uint8_t*>(address),
          util::kPpcContextEncodedSize),
      state);
}

void WriteDispatcherContext(Memory* memory, uint32_t address,
                            uint32_t control_pc, uint32_t function_entry,
                            uint32_t establisher_frame,
                            uint32_t context_record) {
  WriteGuest32(memory, address + util::kDispatcherContextControlPcOffset,
               control_pc);
  WriteGuest32(memory, address + util::kDispatcherContextFunctionEntryOffset,
               function_entry);
  WriteGuest32(memory, address + util::kDispatcherContextEstablisherFrameOffset,
               establisher_frame);
  WriteGuest32(memory, address + util::kDispatcherContextContextRecordOffset,
               context_record);
}

// The registers of a kernel export's guest caller as they will be when the
// export returns: IAR is the return address in LR, r1 the caller's.
PpcRegisterState CaptureCallerState(const cpu::ppc::PPCContext& context) {
  UserModeIntegerContext integer;
  SaveUserModeIntegerContext(context, integer);
  PpcRegisterState state;
  state.context_flags = util::kPpcContextFull;
  state.msr = static_cast<uint32_t>(context.msr);
  state.iar = integer.link_register;
  state.lr = integer.link_register;
  state.ctr = integer.count_register;
  state.gpr = integer.gpr;
  state.cr = integer.condition_register;
  state.xer = integer.xer;
  state.fpscr = context.fpscr.value;
  for (size_t i = 0; i < state.fpr.size(); ++i) {
    std::memcpy(&state.fpr[i], &context.f[i], sizeof(uint64_t));
  }
  return state;
}

// Loads the fields context_flags selects. LR is set to link_register; MSR is
// left alone, and FPSCR too unless restore_fpscr (the reentry landing
// reloads the host floating-point state from it).
void ApplyState(const PpcRegisterState& state, uint32_t link_register,
                bool restore_fpscr, cpu::ppc::PPCContext* context) {
  UserModeIntegerContext integer;
  SaveUserModeIntegerContext(*context, integer);
  if (state.context_flags & util::kPpcContextInteger) {
    integer.gpr = state.gpr;
    integer.condition_register = state.cr;
    integer.xer = state.xer;
  }
  if (state.context_flags & util::kPpcContextControl) {
    integer.count_register = state.ctr;
  }
  integer.link_register = link_register;
  ApplyUserModeIntegerContext(integer, *context);
  if (state.context_flags & util::kPpcContextFloatingPoint) {
    for (size_t i = 0; i < state.fpr.size(); ++i) {
      std::memcpy(&context->f[i], &state.fpr[i], sizeof(uint64_t));
    }
    if (restore_fpscr) {
      context->fpscr.value = state.fpscr;
    }
  }
}

// Reserves size bytes below sp (the caller's r1, or scratch already taken
// below it), 16-byte aligned.
bool AllocateGuestFrame(const XThread& thread, const GuestReader& reader,
                        uint32_t sp, uint32_t size, uint32_t* frame) {
  if (sp < size + kStackLimitMargin) {
    return false;
  }
  const uint32_t base = (sp - size) & ~uint32_t(0xF);
  // On the thread's own stack (not a fiber's) keep off its guard page.
  if (sp > thread.stack_limit() && sp <= thread.stack_base() &&
      base < thread.stack_limit() + kStackLimitMargin) {
    return false;
  }
  if (!reader.IsAccessible(base, sp - base, true)) {
    return false;
  }
  *frame = base;
  return true;
}

// Calls a guest routine with r1 = call_sp, r3... = args and r12 = r12, as
// the kernel calls language handlers and __C_specific_handler calls funclets.
bool CallGuest(XThread& thread, cpu::ppc::PPCContext* context, uint32_t call_sp,
               uint32_t routine, std::initializer_list<uint32_t> args,
               uint32_t r12, GuestCall call, uint32_t* result) {
  if (!routine) {
    return false;
  }
  if (t_guest_call_count >= kMaxGuestCalls) {
    XELOGE("Guest SEH: handlers nested too deeply to call {:08X}", routine);
    return false;
  }
  cpu::ThreadState* thread_state = thread.thread_state();
  call.call_sp = call_sp;
  call.callee_depth = thread_state->guest_dispatch_depth() + 1;
  const uint32_t index = t_guest_call_count++;
  t_guest_calls[index] = call;

  const uint64_t saved_sp = context->r[1];
  size_t arg_index = 0;
  for (uint32_t arg : args) {
    context->r[3 + arg_index++] = arg;
  }
  context->r[12] = r12;
  context->r[1] = call_sp;
  const bool ran = kernel_state()->processor()->Execute(thread_state, routine);
  context->r[1] = saved_sp;
  t_guest_call_count = index;
  if (!ran) {
    XELOGE("Guest SEH: could not call the guest routine at {:08X}", routine);
    return false;
  }
  *result = static_cast<uint32_t>(context->r[3]);
  return true;
}

// ---------------------------------------------------------------------------
// Frame walking.

struct Walk {
  PpcRegisterState state;      // registers at control_pc
  uint32_t control_pc = 0;     // the call instruction in a caller frame
  uint32_t segment_depth = 0;  // dispatch depth of the frames being walked
  uint32_t first_call_sp = 0;  // call_sp of the first call crossed, or 0
};

struct WalkFrame {
  util::PpcRuntimeFunction function;
  util::PpcUnwindResult unwind;
  uint32_t handler = 0;  // the language handler, or 0
};

std::optional<util::PpcGuestRange> FindExceptionTable(const GuestReader& reader,
                                                      uint32_t pc) {
  auto* module = dynamic_cast<cpu::XexModule*>(
      kernel_state()->processor()->LookupModule(pc));
  if (!module) {
    return std::nullopt;
  }
  if (auto directory =
          util::FindPeExceptionDirectory(reader, module->base_address())) {
    return directory;
  }
  if (const PESection* pdata = module->GetPESection(".pdata")) {
    return util::PpcGuestRange{pdata->address, pdata->raw_size};
  }
  return std::nullopt;
}

// Unwinds the frame at walk.control_pc. False ends the walk: no .pdata
// covers the address (another module, a host trampoline) or the frame is
// implausible.
bool ReadFrame(const GuestReader& reader, const Walk& walk, WalkFrame* frame) {
  const std::optional<util::PpcGuestRange> table =
      FindExceptionTable(reader, walk.control_pc);
  if (!table) {
    return false;
  }
  const std::optional<util::PpcRuntimeFunction> function =
      util::LookupPpcRuntimeFunction(reader, *table, walk.control_pc);
  if (!function || function->is_millicode()) {
    return false;
  }
  frame->function = *function;
  if (!util::PpcVirtualUnwind(reader, *function, walk.control_pc, walk.state,
                              &frame->unwind)) {
    XELOGW("Guest SEH: cannot unwind the frame at {:08X} (r1 {:08X})",
           walk.control_pc, static_cast<uint32_t>(walk.state.gpr[1]));
    return false;
  }
  if (frame->unwind.establisher_frame & 3) {
    XELOGW("Guest SEH: misaligned frame {:08X} at {:08X}",
           frame->unwind.establisher_frame, walk.control_pc);
    return false;
  }
  frame->handler = 0;
  if (function->has_handler() &&
      !reader.Read32(function->handler_address(), &frame->handler)) {
    frame->handler = 0;
  }
  return true;
}

enum class WalkAdvance { kNext, kCrossed, kEnd };

// Moves the walk to the caller of the frame just read. At a routine host
// code called, continues from that call's recorded registers (crossed).
WalkAdvance AdvanceWalk(const GuestReader& reader, Memory* memory, Walk* walk,
                        const WalkFrame& frame, GuestCall* crossed) {
  const PpcRegisterState& caller = frame.unwind.caller;
  if (caller.lr == kHostCallReturnAddress) {
    const GuestCall* call = FindGuestCall(frame.unwind.establisher_frame, true);
    if (!call || !call->continuation_state) {
      return WalkAdvance::kEnd;
    }
    *crossed = *call;
    PpcRegisterState state;
    if (!ReadContext(reader, memory, crossed->continuation_state, &state) ||
        state.iar < 4) {
      return WalkAdvance::kEnd;
    }
    walk->state = state;
    walk->control_pc = state.iar - 4;
    walk->segment_depth = crossed->continuation_depth;
    if (!walk->first_call_sp) {
      walk->first_call_sp = crossed->call_sp;
    }
    return WalkAdvance::kCrossed;
  }
  if (caller.lr < 4 || (caller.gpr[1] == walk->state.gpr[1] &&
                        caller.lr - 4 == walk->control_pc)) {
    return WalkAdvance::kEnd;
  }
  walk->state = caller;
  walk->control_pc = caller.lr - 4;
  return WalkAdvance::kNext;
}

// Whether the unwind will reach target_frame, checked before any handler
// runs so that an unwind that cannot complete changes nothing.
bool FindUnwindTarget(const GuestReader& reader, Memory* memory, Walk walk,
                      uint32_t target_frame) {
  if (!target_frame) {
    return false;
  }
  for (uint32_t i = 0; i < kMaxWalkFrames; ++i) {
    WalkFrame frame;
    if (!ReadFrame(reader, walk, &frame) ||
        frame.unwind.establisher_frame > target_frame) {
      return false;
    }
    if (frame.unwind.establisher_frame == target_frame) {
      return true;
    }
    GuestCall crossed;
    const WalkAdvance advance =
        AdvanceWalk(reader, memory, &walk, frame, &crossed);
    if (advance == WalkAdvance::kEnd ||
        (advance == WalkAdvance::kCrossed && crossed.unwind)) {
      return false;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Unwinding.

// Where an unwind can end by returning through the import thunk of the
// kernel call that started it: the guest frames of that call's caller, from
// floor_sp up to the first host call the walk crossed.
struct SameLevelTransfer {
  bool allowed = false;
  uint32_t floor_sp = 0;
  // 0: RtlUnwind's own thunk. Otherwise the raise state of the dispatch the
  // target is handed to (RtlRaiseException's thunk).
  uint32_t dispatch_owner = 0;
};

void TransferToUnwindTarget(XThread& thread, cpu::ppc::PPCContext* context,
                            const PpcRegisterState& target,
                            const SameLevelTransfer& same_level,
                            const Walk& walk, uint32_t code,
                            uint32_t target_frame) {
  const uint32_t target_sp = (target.context_flags & util::kPpcContextInteger)
                                 ? static_cast<uint32_t>(target.gpr[1])
                                 : same_level.floor_sp;
  if (same_level.allowed && target_sp >= same_level.floor_sp &&
      (!walk.first_call_sp || target_sp < walk.first_call_sp)) {
    if (!same_level.dispatch_owner) {
      // RtlUnwind returns; its thunk's blr branches to the target.
      ApplyState(target, target.iar, false, context);
    } else {
      // __C_specific_handler returns to the dispatcher, which applies this.
      t_pending_transfer.owner = same_level.dispatch_owner;
      t_pending_transfer.state = target;
    }
    XELOGD("Guest SEH: exception {:08X} unwinds to {:08X} (frame {:08X})", code,
           target.iar, target_frame);
    return;
  }
  // Host frames lie between here and the target. Reentry abandons them and
  // resumes the target from the thread's dispatch loop, which runs only the
  // outermost guest dispatch.
#if XE_PLATFORM_WIN32
  // The landing resets the depth exactly on Windows only.
  if (walk.segment_depth != 1) {
    ReportSehFailure(SehFailure::kNestedHostCall, code, target.iar,
                     target_frame);
    return;
  }
#endif
  const uint32_t link_register =
      (target.context_flags & util::kPpcContextControl)
          ? target.lr
          : static_cast<uint32_t>(context->lr);
  ApplyState(target, link_register, true, context);
  t_guest_call_count = 0;
  t_pending_transfer.owner = 0;
  XELOGD(
      "Guest SEH: exception {:08X} resumes {:08X} (frame {:08X}, r1 {:08X}) "
      "by reentry",
      code, target.iar, target_frame, target_sp);
  thread.Reenter(target.iar);
}

// RtlUnwind. dispatch_call is the call that entered an in-host
// __C_specific_handler unwinding for a dispatch (the unwind then starts where
// that exception was raised); null for a guest RtlUnwind call (it starts at
// the caller). Its scratch goes below scratch_top (at or below r1). Returns
// when the transfer is left to the import thunk or the dispatcher, when
// nothing was unwound (no target found), or after a failure was reported.
void UnwindGuestFrames(cpu::ppc::PPCContext* context, uint32_t scratch_top,
                       uint32_t target_frame, uint32_t target_ip,
                       uint32_t exception_record, uint32_t return_value,
                       const GuestCall* dispatch_call) {
  XThread* thread = XThread::GetCurrentThread();
  if (!thread || !thread->is_guest_thread() || !thread->thread_state() ||
      thread->thread_state()->context() != context) {
    XELOGE("RtlUnwind({:08X}, {:08X}): not on a guest thread; ignored",
           target_frame, target_ip);
    return;
  }
  Memory* memory = kernel_memory();
  GuestReader reader(memory);
  const uint32_t depth = thread->thread_state()->guest_dispatch_depth();
  if (!dispatch_call) {
    PruneAbandonedGuestCalls(depth);
  }
  const GuestCall origin = dispatch_call ? *dispatch_call : GuestCall();
  const uint32_t caller_sp = static_cast<uint32_t>(context->r[1]);

  uint32_t frame;
  if (!AllocateGuestFrame(*thread, reader, std::min(scratch_top, caller_sp),
                          kUnwindFrameSize, &frame)) {
    XELOGE("RtlUnwind({:08X}, {:08X}): no guest stack for the unwind",
           target_frame, target_ip);
    return;
  }
  const uint32_t context_record = frame + kUnwindContextOffset;
  const uint32_t start_state = frame + kUnwindStartStateOffset;
  const uint32_t dispatcher_context = frame + kUnwindDispatcherContextOffset;

  PpcRegisterState start;
  uint32_t start_depth = depth;
  if (dispatch_call) {
    if (!ReadContext(reader, memory, origin.continuation_state, &start)) {
      XELOGE("RtlUnwind: the dispatch's raise state is unreadable");
      return;
    }
    start_depth = origin.continuation_depth;
  } else {
    start = CaptureCallerState(*context);
  }
  if (start.iar < 4) {
    return;
  }

  if (!exception_record) {
    exception_record = frame + kUnwindRecordOffset;
    std::memset(memory->TranslateVirtual(exception_record), 0,
                util::kExceptionRecordSize);
    WriteGuest32(memory, exception_record + util::kExceptionRecordCodeOffset,
                 util::kStatusUnwind);
    WriteGuest32(memory, exception_record + util::kExceptionRecordAddressOffset,
                 start.iar);
  } else if (!reader.IsAccessible(exception_record, util::kExceptionRecordSize,
                                  true)) {
    XELOGE("RtlUnwind: unreadable exception record {:08X}", exception_record);
    return;
  }
  uint32_t code = 0;
  reader.Read32(exception_record + util::kExceptionRecordCodeOffset, &code);

  Walk walk;
  walk.state = start;
  walk.control_pc = start.iar - 4;
  walk.segment_depth = start_depth;
  if (!FindUnwindTarget(reader, memory, walk, target_frame)) {
    XELOGE(
        "RtlUnwind({:08X}, {:08X}): no frame from {:08X} reaches the target; "
        "returning without unwinding",
        target_frame, target_ip, start.iar);
    return;
  }

  SameLevelTransfer same_level;
  if (!dispatch_call) {
    same_level.allowed = true;
    same_level.floor_sp = caller_sp;
  } else if (origin.language_handler) {
    same_level.allowed = true;
    same_level.floor_sp = static_cast<uint32_t>(start.gpr[1]);
    same_level.dispatch_owner = origin.continuation_state;
  }

  std::memset(memory->TranslateVirtual(context_record), 0,
              util::kPpcContextSize);
  WriteContext(memory, start_state, start);
  // Termination handlers called from here: a walk crossing one continues
  // where this unwind started.
  GuestCall handler_call;
  handler_call.continuation_state =
      dispatch_call ? origin.continuation_state : start_state;
  handler_call.continuation_depth = start_depth;
  handler_call.nested_establisher =
      dispatch_call ? origin.nested_establisher : 0;
  handler_call.unwind = true;

  const uint32_t flags = util::kExceptionUnwinding;
  for (uint32_t i = 0; i < kMaxWalkFrames; ++i) {
    WalkFrame walk_frame;
    if (!ReadFrame(reader, walk, &walk_frame)) {
      break;
    }
    const uint32_t establisher = walk_frame.unwind.establisher_frame;
    if (establisher > target_frame) {
      break;
    }
    const bool is_target = establisher == target_frame;
    PpcRegisterState target_state = walk.state;
    target_state.context_flags = util::kPpcContextFull;
    if (walk_frame.handler && walk_frame.unwind.in_function) {
      PpcRegisterState handler_state = walk.state;
      handler_state.context_flags = util::kPpcContextFull;
      handler_state.iar = target_ip;
      handler_state.gpr[3] = return_value;
      WriteContext(memory, context_record, handler_state);
      WriteGuest32(memory, exception_record + util::kExceptionRecordFlagsOffset,
                   flags | (is_target ? util::kExceptionTargetUnwind : 0));
      WriteDispatcherContext(memory, dispatcher_context, walk.control_pc,
                             walk_frame.function.entry_address, establisher,
                             context_record);
      uint32_t disposition = 0;
      if (!CallGuest(*thread, context, frame, walk_frame.handler,
                     {exception_record, establisher, context_record,
                      dispatcher_context},
                     establisher, handler_call, &disposition)) {
        ReportSehFailure(SehFailure::kHandlerNotRun, code, target_ip,
                         target_frame);
        return;
      }
      if (disposition != util::kExceptionContinueSearch) {
        ReportSehFailure(
            disposition == util::kExceptionCollidedUnwindDisposition
                ? SehFailure::kCollidedUnwind
                : SehFailure::kInvalidDisposition,
            code, target_ip, target_frame);
        return;
      }
      // The target frame's handler may replace the context that is resumed
      // (the C++ runtime puts back the one its _UnwindNestedFrames captured).
      if (is_target &&
          !ReadContext(reader, memory, context_record, &target_state)) {
        ReportSehFailure(SehFailure::kTargetLost, code, target_ip,
                         target_frame);
        return;
      }
    }
    if (is_target) {
      target_state.iar = target_ip;
      target_state.gpr[3] = return_value;
      TransferToUnwindTarget(*thread, context, target_state, same_level, walk,
                             code, target_frame);
      return;
    }
    GuestCall crossed;
    const WalkAdvance advance =
        AdvanceWalk(reader, memory, &walk, walk_frame, &crossed);
    if (advance == WalkAdvance::kEnd) {
      break;
    }
    if (advance == WalkAdvance::kCrossed && crossed.unwind) {
      ReportSehFailure(SehFailure::kCollidedUnwind, code, target_ip,
                       target_frame);
      return;
    }
  }
  ReportSehFailure(SehFailure::kTargetLost, code, target_ip, target_frame);
}

// ---------------------------------------------------------------------------
// __C_specific_handler funclet calls.

struct FilterCall {
  XThread* thread;
  cpu::ppc::PPCContext* context;
  uint32_t call_sp;
  uint32_t exception_pointers;
  uint32_t establisher_frame;
  GuestCall call;
};

int32_t EvaluateFilter(void* opaque, const util::CScopeRecord& scope) {
  auto* filter = static_cast<FilterCall*>(opaque);
  uint32_t value = 0;
  if (!CallGuest(*filter->thread, filter->context, filter->call_sp,
                 scope.handler_address,
                 {filter->exception_pointers, filter->establisher_frame},
                 filter->establisher_frame, filter->call, &value)) {
    return 0;  // EXCEPTION_CONTINUE_SEARCH
  }
  return static_cast<int32_t>(value);
}

struct TerminationCall {
  XThread* thread;
  cpu::ppc::PPCContext* context;
  Memory* memory;
  uint32_t call_sp;
  uint32_t dispatcher_context;
  uint32_t establisher_frame;
  GuestCall call;
};

void RunTerminationHandler(void* opaque, const util::CScopeRecord& scope) {
  auto* termination = static_cast<TerminationCall*>(opaque);
  // A collided unwind restarts the scan past the scopes already run.
  WriteGuest32(
      termination->memory,
      termination->dispatcher_context + util::kDispatcherContextControlPcOffset,
      scope.end_address + 4);
  uint32_t ignored = 0;
  // AbnormalTermination() is TRUE when a termination handler runs during an
  // unwind.
  CallGuest(*termination->thread, termination->context, termination->call_sp,
            scope.handler_address, {1u, termination->establisher_frame},
            termination->establisher_frame, termination->call, &ignored);
}

}  // namespace

bool DispatchGuestException(uint32_t exception_record,
                            cpu::ppc::PPCContext* ppc_context) {
  if (!cvars::guest_exception_dispatch &&
      kernel_state()->title_id() != kXN_2002) {
    return false;
  }
  XThread* thread = XThread::GetCurrentThread();
  if (!exception_record || !thread || !thread->is_guest_thread() ||
      !thread->thread_state() ||
      thread->thread_state()->context() != ppc_context) {
    return false;
  }
  Memory* memory = kernel_memory();
  GuestReader reader(memory);
  const uint32_t depth = thread->thread_state()->guest_dispatch_depth();
  PruneAbandonedGuestCalls(depth);
  if (!t_guest_call_count) {
    t_pending_transfer.owner = 0;  // left by an abandoned dispatch
  }
  const uint32_t caller_sp = static_cast<uint32_t>(ppc_context->r[1]);

  uint32_t frame;
  if (!reader.IsAccessible(exception_record, util::kExceptionRecordSize,
                           true) ||
      !AllocateGuestFrame(*thread, reader, caller_sp, kDispatchFrameSize,
                          &frame)) {
    XELOGE("Guest SEH: cannot dispatch exception record {:08X}",
           exception_record);
    return false;
  }
  const uint32_t context_record = frame + kDispatchContextOffset;
  const uint32_t raise_state = frame + kDispatchRaiseStateOffset;
  const uint32_t dispatcher_context = frame + kDispatchDispatcherContextOffset;

  const PpcRegisterState raise = CaptureCallerState(*ppc_context);
  if (raise.iar < 4) {
    return false;
  }
  std::memset(memory->TranslateVirtual(context_record), 0,
              util::kPpcContextSize);
  WriteContext(memory, context_record, raise);
  WriteContext(memory, raise_state, raise);
  // As on the console, the exception address is RtlRaiseException's return
  // address.
  WriteGuest32(memory, exception_record + util::kExceptionRecordAddressOffset,
               raise.iar);
  uint32_t code = 0;
  uint32_t record_flags = 0;
  reader.Read32(exception_record + util::kExceptionRecordCodeOffset, &code);
  reader.Read32(exception_record + util::kExceptionRecordFlagsOffset,
                &record_flags);
  XELOGD("Guest SEH: dispatching exception {:08X} raised at {:08X}", code,
         raise.iar);

  Walk walk;
  walk.state = raise;
  walk.control_pc = raise.iar - 4;
  walk.segment_depth = depth;
  uint32_t flags = record_flags & util::kExceptionNoncontinuable;
  uint32_t nested_frame = 0;
  for (uint32_t i = 0; i < kMaxWalkFrames; ++i) {
    WalkFrame walk_frame;
    if (!ReadFrame(reader, walk, &walk_frame)) {
      break;
    }
    const uint32_t establisher = walk_frame.unwind.establisher_frame;
    if (walk_frame.handler && walk_frame.unwind.in_function) {
      WriteGuest32(memory, exception_record + util::kExceptionRecordFlagsOffset,
                   flags);
      WriteDispatcherContext(memory, dispatcher_context, walk.control_pc,
                             walk_frame.function.entry_address, establisher,
                             context_record);
      GuestCall handler_call;
      handler_call.continuation_state = raise_state;
      handler_call.continuation_depth = depth;
      handler_call.nested_establisher = establisher;
      handler_call.language_handler = true;
      uint32_t disposition = 0;
      if (!CallGuest(*thread, ppc_context, frame, walk_frame.handler,
                     {exception_record, establisher, context_record,
                      dispatcher_context},
                     establisher, handler_call, &disposition)) {
        return false;
      }
      if (t_pending_transfer.owner == raise_state) {
        // The handler unwound to an __except block in these frames:
        // RtlRaiseException returns there through its import thunk.
        t_pending_transfer.owner = 0;
        const PpcRegisterState& target = t_pending_transfer.state;
        ApplyState(target, target.iar, false, ppc_context);
        return true;
      }
      reader.Read32(exception_record + util::kExceptionRecordFlagsOffset,
                    &record_flags);
      flags |= record_flags & util::kExceptionNoncontinuable;
      // As RtlDispatchException does: the frame that was being dispatched
      // when the nested exception was raised is itself still searched as
      // nested; the frames past it are not.
      if (nested_frame == establisher) {
        flags &= ~util::kExceptionNestedCall;
        nested_frame = 0;
      }
      if (disposition == util::kExceptionContinueExecution) {
        if (flags & util::kExceptionNoncontinuable) {
          XELOGE(
              "Guest SEH: handler {:08X} continued noncontinuable exception "
              "{:08X}",
              walk_frame.handler, code);
          return false;
        }
        PpcRegisterState resumed;
        if (!ReadContext(reader, memory, context_record, &resumed)) {
          return false;
        }
        const uint32_t resume_ip =
            (resumed.context_flags & util::kPpcContextControl) ? resumed.iar
                                                               : raise.iar;
        const uint32_t resume_sp =
            (resumed.context_flags & util::kPpcContextInteger)
                ? static_cast<uint32_t>(resumed.gpr[1])
                : caller_sp;
        if (resume_sp < caller_sp) {
          XELOGE(
              "Guest SEH: handler {:08X} resumed below its caller's stack "
              "(r1 {:08X} < {:08X})",
              walk_frame.handler, resume_sp, caller_sp);
          return false;
        }
        // RtlRaiseException returns through its import thunk, whose blr
        // branches to resume_ip.
        ApplyState(resumed, resume_ip, false, ppc_context);
        XELOGD("Guest SEH: handler {:08X} resumed exception {:08X} at {:08X}",
               walk_frame.handler, code, resume_ip);
        return true;
      }
      if (disposition == util::kExceptionNestedException) {
        uint32_t nested = 0;
        reader.Read32(
            dispatcher_context + util::kDispatcherContextEstablisherFrameOffset,
            &nested);
        flags |= util::kExceptionNestedCall;
        nested_frame = std::max(nested_frame, nested);
      } else if (disposition != util::kExceptionContinueSearch) {
        XELOGE("Guest SEH: handler {:08X} returned invalid disposition {}",
               walk_frame.handler, disposition);
        return false;
      }
    }
    GuestCall crossed;
    const WalkAdvance advance =
        AdvanceWalk(reader, memory, &walk, walk_frame, &crossed);
    if (advance == WalkAdvance::kEnd) {
      break;
    }
    // Raised inside a handler, filter or termination handler that a dispatch
    // is waiting on: the frames up to the one being dispatched are searched
    // again as nested.
    if (advance == WalkAdvance::kCrossed && crossed.nested_establisher) {
      flags |= util::kExceptionNestedCall;
      nested_frame = std::max(nested_frame, crossed.nested_establisher);
    }
  }
  XELOGD("Guest SEH: no handler took exception {:08X}", code);
  return false;
}

void RtlCaptureContext_entry(dword_t context_record,
                             const ppc_context_t& context) {
  Memory* memory = kernel_memory();
  GuestReader reader(memory);
  if (!context_record ||
      !reader.IsAccessible(context_record, util::kPpcContextEncodedSize,
                           true)) {
    return;
  }
  WriteContext(memory, context_record, CaptureCallerState(*context.value()));
}
DECLARE_XBOXKRNL_EXPORT1(RtlCaptureContext, kDebug, kImplemented);

void RtlUnwind_entry(dword_t target_frame, dword_t target_ip,
                     dword_t exception_record, dword_t return_value,
                     const ppc_context_t& context) {
  UnwindGuestFrames(context.value(), static_cast<uint32_t>(context->r[1]),
                    target_frame, target_ip, exception_record, return_value,
                    nullptr);
}
DECLARE_XBOXKRNL_EXPORT2(RtlUnwind, kDebug, kImplemented, kImportant);

// The C language handler, following the documented scope-table semantics
// (as in the PowerPC NT runtime): filters run for an exception dispatch,
// termination handlers for an unwind.
dword_result_t __C_specific_handler_entry(dword_t exception_record,
                                          dword_t establisher_frame,
                                          dword_t context_record,
                                          dword_t dispatcher_context,
                                          const ppc_context_t& context) {
  Memory* memory = kernel_memory();
  GuestReader reader(memory);
  XThread* thread = XThread::GetCurrentThread();
  uint32_t flags;
  uint32_t control_pc;
  uint32_t function_entry;
  uint32_t begin_address;
  uint32_t scope_table;
  if (!thread || !thread->thread_state() ||
      thread->thread_state()->context() != context.value() ||
      !reader.Read32(exception_record + util::kExceptionRecordFlagsOffset,
                     &flags) ||
      !reader.Read32(
          dispatcher_context + util::kDispatcherContextControlPcOffset,
          &control_pc) ||
      !reader.Read32(
          dispatcher_context + util::kDispatcherContextFunctionEntryOffset,
          &function_entry) ||
      !reader.Read32(function_entry, &begin_address) ||
      !reader.Read32(begin_address - 4, &scope_table)) {
    XELOGE("__C_specific_handler: unreadable dispatch state");
    return util::kExceptionContinueSearch;
  }
  // The call that entered this handler, when the dispatcher or an unwind
  // here did (the import thunk adds no frame). Calls a reentry abandoned
  // must not be taken for it.
  PruneAbandonedGuestCalls(thread->thread_state()->guest_dispatch_depth());
  const GuestCall* entered_by =
      FindGuestCall(static_cast<uint32_t>(context->r[1]), false);
  const GuestCall caller_call = entered_by ? *entered_by : GuestCall();

  uint32_t frame;
  if (!AllocateGuestFrame(*thread, reader, static_cast<uint32_t>(context->r[1]),
                          kCHandlerFrameSize, &frame)) {
    XELOGE("__C_specific_handler: no guest stack");
    return util::kExceptionContinueSearch;
  }

  if (!(flags & util::kExceptionUnwind)) {
    // EXCEPTION_POINTERS {ExceptionRecord, ContextRecord}.
    WriteGuest32(memory, frame, exception_record);
    WriteGuest32(memory, frame + 4, context_record);
    FilterCall filter{thread,
                      context.value(),
                      frame,
                      frame,
                      static_cast<uint32_t>(establisher_frame),
                      caller_call};
    filter.call.unwind = false;
    filter.call.language_handler = false;
    const util::CScopeDispatchDecision decision = util::DispatchCScopeTable(
        reader, scope_table, control_pc, &EvaluateFilter, &filter);
    switch (decision.action) {
      case util::CScopeDispatchAction::kContinueExecution:
        return util::kExceptionContinueExecution;
      case util::CScopeDispatchAction::kExecuteHandler: {
        uint32_t code = 0;
        reader.Read32(exception_record + util::kExceptionRecordCodeOffset,
                      &code);
        XELOGD(
            "__C_specific_handler: unwinding to __except {:08X}, frame {:08X}",
            decision.scope.jump_target,
            static_cast<uint32_t>(establisher_frame));
        UnwindGuestFrames(context.value(), frame, establisher_frame,
                          decision.scope.jump_target, exception_record, code,
                          entered_by ? &caller_call : nullptr);
        // Back when the dispatcher is to apply the target (it checks before
        // the disposition) or when nothing was unwound.
        return util::kExceptionContinueSearch;
      }
      case util::CScopeDispatchAction::kBadTable:
        XELOGE("__C_specific_handler: unreadable scope table {:08X}",
               scope_table);
        return util::kExceptionContinueSearch;
      case util::CScopeDispatchAction::kContinueSearch:
        return util::kExceptionContinueSearch;
    }
    return util::kExceptionContinueSearch;
  }

  // The unwind's target address: RtlUnwind puts it in the context's IAR.
  uint32_t target_pc = 0;
  reader.Read32(context_record + util::kPpcContextIarOffset, &target_pc);
  TerminationCall termination{thread,
                              context.value(),
                              memory,
                              frame,
                              static_cast<uint32_t>(dispatcher_context),
                              static_cast<uint32_t>(establisher_frame),
                              caller_call};
  termination.call.unwind = true;
  termination.call.language_handler = false;
  if (!util::UnwindCScopeTable(reader, scope_table, control_pc, target_pc,
                               &RunTerminationHandler, &termination)) {
    XELOGE("__C_specific_handler: unreadable scope table {:08X}", scope_table);
  }
  return util::kExceptionContinueSearch;
}
DECLARE_XBOXKRNL_EXPORT1(__C_specific_handler, kDebug, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Seh);

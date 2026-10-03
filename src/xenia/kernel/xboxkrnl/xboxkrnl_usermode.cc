/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_kernel_runtime.h"

#include "xenia/base/byte_order.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_mode_context.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

namespace xe::kernel::xboxkrnl {

dword_result_t KeCreateUserMode_entry(dword_t selector,
                                      dword_t control_page_address) {
  return kernel_state()->user_mode_kernel_runtime()->CreateMapping(
      selector, control_page_address);
}
DECLARE_XBOXKRNL_EXPORT2(KeCreateUserMode, kMemory, kImplemented, kImportant);

void KeDeleteUserMode_entry(dword_t selector) {
  kernel_state()->user_mode_kernel_runtime()->DeleteMapping(selector);
}
DECLARE_XBOXKRNL_EXPORT2(KeDeleteUserMode, kMemory, kImplemented, kImportant);

dword_result_t KeEnterUserMode_entry(dword_t context_address,
                                     dword_t handler_address,
                                     dword_t save_area_end,
                                     dword_t companion_state,
                                     const ppc_context_t& context) {
  // Success enters user mode and never returns here; failures do.
  return kernel_state()->user_mode_kernel_runtime()->Enter(
      context_address, handler_address, save_area_end, companion_state,
      context.value());
}
DECLARE_XBOXKRNL_EXPORT2(KeEnterUserMode, kMemory, kImplemented, kImportant);

void KeFlushUserModeCurrentTb_entry(dword_t selector,
                                    const ppc_context_t& context) {
  kernel_state()->user_mode_kernel_runtime()->Flush(selector, context.value());
}
// Called hundreds of times a second.
DECLARE_XBOXKRNL_EXPORT2(KeFlushUserModeCurrentTb, kMemory, kImplemented,
                         kHighFrequency);

void KeFlushUserModeTb_entry(dword_t selector, const ppc_context_t& context) {
  // Both flush variants flush the selected mapping.
  kernel_state()->user_mode_kernel_runtime()->Flush(selector, context.value());
}
DECLARE_XBOXKRNL_EXPORT2(KeFlushUserModeTb, kMemory, kImplemented,
                         kHighFrequency);

void KeFlushCurrentEntireTb_entry(const ppc_context_t& context) {
  kernel_state()->user_mode_kernel_runtime()->FlushEntireTb(context.value());
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushCurrentEntireTb, kMemory, kImplemented);

void KeLeaveUserMode_entry(const ppc_context_t& context) {
  kernel_state()->user_mode_kernel_runtime()->Leave(context.value());
}
DECLARE_XBOXKRNL_EXPORT2(KeLeaveUserMode, kMemory, kImplemented, kImportant);

// CONTEXT+0 flags selecting the fields a frame conversion copies.
constexpr uint32_t kContextFlagsControl = 0x1;  // IAR, LR, CTR
constexpr uint32_t kContextFlagsInteger = 0x4;  // r0-r31, CR, XER

// Copies the trap frame fields the flags select (user_mode_context.h's
// trap-frame layout) into a CONTEXT. Other fields and the UserModeControl are
// left as they are.
void KeContextFromKframes_entry(lpvoid_t kframes, lpvoid_t context) {
  if (!kframes || !context) {
    return;
  }
  std::span<uint8_t> context_bytes(context.as<uint8_t*>(),
                                   kUserModeContextMinimumSize);
  UserModeIntegerContext frame;
  UserModeIntegerContextResult current =
      DecodeUserModeIntegerContext(context_bytes);
  if (DecodeUserModeTrapFrame(
          std::span<const uint8_t>(kframes.as<const uint8_t*>(),
                                   kUserModeTrapFrameSize),
          frame) != UserModeContextCodecError::kNone ||
      !current) {
    return;
  }
  const uint32_t flags = xe::load_and_swap<uint32_t>(context_bytes.data());
  UserModeIntegerContext& result = current.context;
  if (flags & kContextFlagsControl) {
    result.entry_address = frame.entry_address;
    result.link_register = frame.link_register;
    result.count_register = frame.count_register;
  }
  if (flags & kContextFlagsInteger) {
    result.gpr = frame.gpr;
    result.condition_register = frame.condition_register;
    result.xer = frame.xer;
  }
  EncodeUserModeIntegerContext(result, context_bytes);
}
DECLARE_XBOXKRNL_EXPORT1(KeContextFromKframes, kMemory, kImplemented);

// The inverse: loads the selected CONTEXT fields into a trap frame.
void KeContextToKframes_entry(lpvoid_t kframes, lpvoid_t context,
                              dword_t flags) {
  if (!kframes || !context) {
    return;
  }
  std::span<uint8_t> frame_bytes(kframes.as<uint8_t*>(),
                                 kUserModeTrapFrameSize);
  UserModeIntegerContextResult source =
      DecodeUserModeIntegerContext(std::span<const uint8_t>(
          context.as<const uint8_t*>(), kUserModeContextMinimumSize));
  UserModeIntegerContext frame;
  if (!source || DecodeUserModeTrapFrame(frame_bytes, frame) !=
                     UserModeContextCodecError::kNone) {
    return;
  }
  if (flags & kContextFlagsControl) {
    frame.entry_address = source.context.entry_address;
    frame.link_register = source.context.link_register;
    frame.count_register = source.context.count_register;
  }
  if (flags & kContextFlagsInteger) {
    frame.gpr = source.context.gpr;
    frame.condition_register = source.context.condition_register;
    frame.xer = source.context.xer;
  }
  EncodeUserModeTrapFrame(frame, frame_bytes);
}
DECLARE_XBOXKRNL_EXPORT1(KeContextToKframes, kMemory, kImplemented);

}  // namespace xe::kernel::xboxkrnl

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(UserMode);

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_kernel_runtime.h"

#include "xenia/kernel/kernel_state.h"
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

}  // namespace xe::kernel::xboxkrnl

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(UserMode);

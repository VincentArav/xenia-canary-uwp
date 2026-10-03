/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_SEH_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_SEH_H_

#include <cstdint>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// RtlDispatchException for RtlRaiseException's codes other than the thread
// naming and C++ ones: walks the guest frames from RtlRaiseException's caller
// and calls their language handlers. Runs only for the XNA Indie Games player
// (kXN_2002) or with --guest_exception_dispatch; otherwise returns false.
//
// Returns true when execution resumes among the caller's own frames (a
// handler continued the exception, or an __except block there takes it): the
// context then holds the resumed registers with LR set to the resume address,
// where RtlRaiseException's import thunk branches. Returns false when no
// handler took the exception. An __except block beyond host frames is entered
// by reentry and never returns here.
bool DispatchGuestException(uint32_t exception_record,
                            cpu::ppc::PPCContext* ppc_context);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_SEH_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_CONTROL_PAGE_H_
#define XENIA_KERNEL_USER_MODE_CONTROL_PAGE_H_

#include <cstdint>

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

inline constexpr uint32_t kUserModeControlPageSize = 0x1000;

// Why a control page was rejected (internal, not guest statuses).
enum class UserModeControlPageDiagnostic {
  kNone,
  kInvalidVirtualAddress,
  kInvalidVirtualRange,
  kPageNotCommitted,
  kPageNotReadWrite,
  kUnsupportedMapping,
  kInvalidPhysicalAddress,
};

// control_page_physical_address is zero on failure.
struct UserModeControlPageResolution {
  UserModeControlPageDiagnostic diagnostic =
      UserModeControlPageDiagnostic::kNone;
  uint32_t control_page_virtual_address = 0;
  uint32_t control_page_physical_address = 0;

  explicit operator bool() const {
    return diagnostic == UserModeControlPageDiagnostic::kNone &&
           control_page_physical_address != 0;
  }
};

// Validates a guest control page and resolves its physical address. Checks,
// in order: address shape, heap range, commitment, read/write access, mapping
// kind, then the physical address.
UserModeControlPageResolution ResolveUserModeControlPage(
    Memory& memory, uint32_t control_page_virtual_address);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_CONTROL_PAGE_H_

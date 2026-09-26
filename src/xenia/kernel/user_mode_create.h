/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_CREATE_H_
#define XENIA_KERNEL_USER_MODE_CREATE_H_

#include <cstdint>

#include "xenia/kernel/user_mode_control_page.h"
#include "xenia/kernel/user_mode_manager.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

// KeCreateUserMode's result. An invalid control page or layout gives
// STATUS_INVALID_PARAMETER; a disabled manager gives no guest status.
struct UserModeCreateFromGuestPageResult {
  UserModeControlPageResolution control_page;
  UserModeCreateMappingResult mapping;

  explicit operator bool() const {
    return control_page && static_cast<bool>(mapping);
  }
};

UserModeCreateFromGuestPageResult CreateUserModeMappingFromGuestPage(
    Memory& memory, UserModeManager& manager, uint32_t selector,
    uint32_t control_page_virtual_address);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_CREATE_H_

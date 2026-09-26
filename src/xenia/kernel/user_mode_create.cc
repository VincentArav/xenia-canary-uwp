/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_create.h"

#include <array>
#include <cstring>

#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe::kernel {

UserModeCreateFromGuestPageResult CreateUserModeMappingFromGuestPage(
    Memory& memory, UserModeManager& manager, uint32_t selector,
    uint32_t control_page_virtual_address) {
  UserModeCreateFromGuestPageResult result;

  // Hold the global lock, which guards memory changes, from validation
  // through publication.
  std::array<uint8_t, kUserModeControlPageSize> control_page_snapshot{};
  {
    auto global_lock = xe::global_critical_region::AcquireDirect();
    result.control_page =
        ResolveUserModeControlPage(memory, control_page_virtual_address);
    if (!result.control_page) {
      // An invalid control page is STATUS_INVALID_PARAMETER.
      result.mapping.guest_status = X_STATUS_INVALID_PARAMETER;
      return result;
    }

    std::memcpy(
        control_page_snapshot.data(),
        memory.TranslateVirtual<const uint8_t*>(control_page_virtual_address),
        control_page_snapshot.size());

    UserModeCreateTimeLayoutDecodeResult decoded_layout =
        DecodeUserModeCreateTimeAddressSpaceLayout(control_page_snapshot);
    if (!decoded_layout) {
      // So is a layout that doesn't decode.
      result.mapping.guest_status = X_STATUS_INVALID_PARAMETER;
      return result;
    }
    result.mapping = manager.CreateMapping(
        selector, result.control_page.control_page_physical_address,
        *decoded_layout.layout);
  }
  return result;
}

}  // namespace xe::kernel

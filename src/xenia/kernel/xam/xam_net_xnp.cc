/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xefu_link.h"
#include "xenia/xbox.h"
#include "xenia/xefu/xefu_link_router.h"

// The Ethernet intercept functions, through which XeFu sends and receives its
// emulated network card's frames. Outside a XeFu session with system link on,
// they do nothing and return r3 unchanged.
namespace xe {
namespace kernel {
namespace xam {

dword_result_t NetDll_XnpEthernetInterceptSetCallbacks_entry(dword_t caller,
                                                             dword_t filter,
                                                             dword_t receive,
                                                             dword_t context,
                                                             dword_t reserved) {
  if (!xefu_link::IsEnabled(kernel_state())) {
    return uint32_t(caller);
  }
  XELOGI("XnpEthernetInterceptSetCallbacks({:08X}, {:08X}, {:08X}, {:08X})",
         uint32_t(filter), uint32_t(receive), uint32_t(context),
         uint32_t(reserved));
  xefu_link::SetCallbacks(kernel_state(), filter, receive, context);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XnpEthernetInterceptSetCallbacks, kNetworking,
                    kImplemented);

dword_result_t NetDll_XnpEthernetInterceptXmit_entry(dword_t caller,
                                                     lpvoid_t frame,
                                                     dword_t length) {
  if (!xefu_link::IsEnabled(kernel_state())) {
    return uint32_t(caller);
  }
  if (!frame || length > xe::xefu::kLinkMaximumFrameSize) {
    return X_ERROR_INVALID_PARAMETER;
  }
  xefu_link::Transmit({frame.as<const uint8_t*>(), uint32_t(length)});
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XnpEthernetInterceptXmit, kNetworking, kImplemented);

// Takes no arguments; r3 is read only to return it unchanged outside XeFu. A
// retail console is on 'PROD'.
dword_result_t XNetLogonGetServiceNetworkID_entry(dword_t r3) {
  if (!kernel_state()->emulator()->mounted_xefu_session()) {
    return uint32_t(r3);
  }
  return 0x50524F44;  // 'PROD'
}
DECLARE_XAM_EXPORT1(XNetLogonGetServiceNetworkID, kNetworking, kImplemented);

// There is no machine account to load; the buffer is zeroed.
constexpr uint32_t kXnpMachineAccountSize = 0x1EC;
dword_result_t NetDll_XnpLoadMachineAccount_entry(dword_t caller,
                                                  lpvoid_t account) {
  if (!kernel_state()->emulator()->mounted_xefu_session()) {
    return uint32_t(caller);
  }
  if (account) {
    account.Zero(kXnpMachineAccountSize);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XnpLoadMachineAccount, kNetworking, kSketchy);

// Not logged on: both statuses are zero. Outside XeFu, r3 is returned
// unchanged.
dword_result_t XNetLogonGetExtendedStatus_entry(lpdword_t status,
                                                lpdword_t extended_status) {
  if (!kernel_state()->emulator()->mounted_xefu_session()) {
    return status.guest_address();
  }
  if (status) {
    *status = 0;
  }
  if (extended_status) {
    *extended_status = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNetLogonGetExtendedStatus, kNetworking, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NetXnp);

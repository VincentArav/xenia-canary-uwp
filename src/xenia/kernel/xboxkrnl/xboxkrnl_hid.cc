/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t HidReadKeys_entry(dword_t unk1, unknown_t unk2, unknown_t unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}
DECLARE_XBOXKRNL_EXPORT1(HidReadKeys, kInput, kStub);

// STATUS_DEVICE_NOT_CONNECTED, which RtlNtStatusToDosError turns into
// ERROR_DEVICE_NOT_CONNECTED.
constexpr X_STATUS kStatusDeviceNotConnected = 0xC000009D;

// Requests to the media remote and the microphone take one request block and
// return NTSTATUS. Neither device is attached: requests fail at once and leave
// the block alone.
dword_result_t RmcDeviceRequest_entry(dword_t request_ptr) {
  return kStatusDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(RmcDeviceRequest, kInput, kStub);

dword_result_t MicDeviceRequest_entry(dword_t request_ptr) {
  return kStatusDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kInput, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Hid);

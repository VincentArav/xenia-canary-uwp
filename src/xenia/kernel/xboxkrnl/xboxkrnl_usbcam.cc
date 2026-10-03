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

dword_result_t XUsbcamCreate_entry(dword_t buffer,
                                   dword_t buffer_size,  // 0x4B000 640x480?
                                   lpdword_t handle_out) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamCreate, kNone, kStub);

dword_result_t XUsbcamGetState_entry() {
  // 0 = not connected.
  // 1 = initialized
  // 2 = connected
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetState, kNone, kStub);

// The camera calls below return Win32 error codes, not NTSTATUS. With no camera
// attached they fail at once with ERROR_DEVICE_NOT_CONNECTED and touch no
// caller memory.

// XUsbcamCreate succeeds without a camera, so destroying what it created does
// too.
dword_result_t XUsbcamDestroy_entry(dword_t handle) { return X_ERROR_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XUsbcamDestroy, kNone, kStub);

dword_result_t XUsbcamSetConfig_entry(dword_t handle, dword_t unknown1,
                                      dword_t unknown2, dword_t value_ptr,
                                      dword_t value_size,
                                      dword_t completion_routine,
                                      dword_t overlapped_ptr) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetConfig, kNone, kStub);

dword_result_t XUsbcamSetView_entry(dword_t handle, dword_t unknown1,
                                    dword_t unknown2, dword_t unknown3,
                                    dword_t completion_routine,
                                    dword_t overlapped_ptr) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetView, kNone, kStub);

// Takes further arguments, the OVERLAPPED among them on the stack.
dword_result_t XUsbcamSetCaptureMode_entry(dword_t handle, dword_t unknown1,
                                           dword_t unknown2_ptr,
                                           dword_t unknown3, dword_t unknown4) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamSetCaptureMode, kNone, kStub);

dword_result_t XUsbcamReadFrame_entry(dword_t handle, dword_t buffer_ptr,
                                      dword_t buffer_size,
                                      dword_t completion_routine,
                                      dword_t overlapped_ptr) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamReadFrame, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Usbcam);

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XEFU_LINK_H_
#define XENIA_KERNEL_XEFU_LINK_H_

#include <cstdint>
#include <span>

namespace xe::kernel {

class KernelState;

// System link for original Xbox games under XeFu: carries the Ethernet frames
// XeFu hands to xam's intercept functions to peers as UDP datagrams (one frame
// each), and delivers arriving frames to XeFu's receive callback in a DPC on
// processor 0. Exists only in a XeFu session with xefu_system_link on, with its
// own host socket.
namespace xefu_link {

// Whether a XeFu session is mounted and system link is enabled.
bool IsEnabled(KernelState* kernel_state);

// NetDll_XnpEthernetInterceptSetCallbacks: registers (or, with all zero,
// unregisters) the guest's frame filter, receive callback and context.
// Starts the link on the first registration.
void SetCallbacks(KernelState* kernel_state, uint32_t filter, uint32_t receive,
                  uint32_t context);

// NetDll_XnpEthernetInterceptXmit: sends one guest frame. False if the link
// is not running or the frame is malformed.
bool Transmit(std::span<const uint8_t> frame);

// Stops the link and releases its guest memory (title reset).
void Shutdown();

}  // namespace xefu_link
}  // namespace xe::kernel

#endif  // XENIA_KERNEL_XEFU_LINK_H_

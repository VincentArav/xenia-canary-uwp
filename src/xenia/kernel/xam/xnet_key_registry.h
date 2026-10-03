/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XNET_KEY_REGISTRY_H_
#define XENIA_KERNEL_XAM_XNET_KEY_REGISTRY_H_

#include <cstdint>

#include "xenia/kernel/xnet.h"

namespace xe {
namespace kernel {
namespace xam {

// What Xenia remembers of the keys a title registered with XNetRegisterKey:
// the system link key (XLiveAPI::GetSystemlinkID) and whether an Xbox Live key
// is registered (EXPLICIT_XBOXLIVE_KEY).
struct XNetRegisteredKeys {
  uint64_t systemlink_id = 0;
  bool explicit_xboxlive_key = false;
};

// XNetReplaceKey(old_xnkid, new_xnkid): the exchange key registered under
// old_xnkid is registered under new_xnkid instead. New system link and Xbox
// Live keys are recorded as XNetRegisterKey records them, and the old key's
// record is dropped unless the new key takes its place.
inline XNetRegisteredKeys ReplaceRegisteredKey(XNetRegisteredKeys keys,
                                               uint64_t old_xnkid,
                                               uint64_t new_xnkid) {
  if (IsSystemlink(new_xnkid)) {
    keys.systemlink_id = new_xnkid;
  } else if (keys.systemlink_id == old_xnkid) {
    keys.systemlink_id = 0;
  }
  if (IsOnlinePeer(new_xnkid)) {
    keys.explicit_xboxlive_key = true;
  } else if (IsOnlinePeer(old_xnkid)) {
    keys.explicit_xboxlive_key = false;
  }
  return keys;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XNET_KEY_REGISTRY_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_LIVE_GUIDE_UI_H_
#define XENIA_KERNEL_XAM_LIVE_GUIDE_UI_H_

#include <cstdint>

#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
namespace live_guide_ui {

// The Xbox Live guide UIs Xenia has no implementation of: XamShowPlayersUI,
// XamShowMessagesUI, XamShowPlayerReviewUI, XamShowMessageComposeUI,
// XamShowGameInviteUI and XamShowFriendRequestUI. None of them takes an
// X_OVERLAPPED; the guide opens asynchronously and the call returns at once.

// What such a UI returns for the user at user_index: only a user signed in to
// Xbox Live can open one, otherwise ERROR_NOT_LOGGED_ON.
inline X_RESULT CheckUser(uint32_t user_index, bool signed_in,
                          bool signed_in_to_live) {
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!signed_in) {
    return X_ERROR_NO_SUCH_USER;
  }
  if (!signed_in_to_live) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  return X_ERROR_SUCCESS;
}

// XamShowMessageComposeUI and XamShowGameInviteUI take an optional list of
// recipient XUIDs: a count of zero needs no list, any other count needs one.
inline bool IsValidRecipientList(uint32_t recipients_ptr,
                                 uint32_t recipient_count) {
  return recipient_count == 0 || recipients_ptr != 0;
}

}  // namespace live_guide_ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_LIVE_GUIDE_UI_H_

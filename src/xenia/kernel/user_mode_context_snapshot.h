/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_CONTEXT_SNAPSHOT_H_
#define XENIA_KERNEL_USER_MODE_CONTEXT_SNAPSHOT_H_

#include <cstdint>
#include <string_view>

#include "xenia/kernel/user_mode_context.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

// Why a user-mode context couldn't be read (internal, not guest statuses).
enum class UserModeContextSnapshotDiagnostic {
  kNone,
  kAddressRangeWrap,
  kUnmappedRange,
  kCrossesHeapBoundary,
  kUnsupportedMapping,
  kRangeNotCommitted,
  kRangeNotReadable,
  kRangeNotWritable,
  kDecodeFailed,
};

// The decoded context fields.
struct UserModeContextSnapshotResult {
  UserModeContextSnapshotDiagnostic diagnostic =
      UserModeContextSnapshotDiagnostic::kNone;
  uint32_t context_virtual_address = 0;
  uint32_t context_end_virtual_address = 0;
  UserModeIntegerContext context;

  explicit operator bool() const {
    return diagnostic == UserModeContextSnapshotDiagnostic::kNone;
  }
};

// Copies and decodes the 0x22C-byte prefix of a guest user-mode context. The
// range must lie in one heap, with every page committed and readable; MMIO is
// rejected.
UserModeContextSnapshotResult ResolveUserModeContextSnapshot(
    Memory& memory, uint32_t context_virtual_address);

struct UserModeContextWriteResult {
  UserModeContextSnapshotDiagnostic diagnostic =
      UserModeContextSnapshotDiagnostic::kNone;
  uint32_t context_virtual_address = 0;
  uint32_t context_end_virtual_address = 0;

  explicit operator bool() const {
    return diagnostic == UserModeContextSnapshotDiagnostic::kNone;
  }
};

// Writes the integer and UserModeControl fields back to a guest user-mode
// context, keeping its other bytes. Validation and the copy happen under the
// global lock.
UserModeContextWriteResult WriteUserModeIntegerContextSnapshot(
    Memory& memory, uint32_t context_virtual_address,
    const UserModeIntegerContext& context);

std::string_view UserModeContextSnapshotDiagnosticToString(
    UserModeContextSnapshotDiagnostic diagnostic);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_CONTEXT_SNAPSHOT_H_

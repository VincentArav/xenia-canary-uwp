/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_ENTER_H_
#define XENIA_KERNEL_USER_MODE_ENTER_H_

#include <cstdint>
#include <optional>
#include <string_view>

#include "xenia/kernel/user_mode_context_snapshot.h"
#include "xenia/kernel/user_mode_manager.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

// The UserModeControl word (context +0x228): bit 0 picks the SLB table half,
// bit 1 the Create selector (0 or 1) and bit 2 the target MSR LE bit. Other
// bits are kept in raw_value.
struct UserModeControlSelection {
  uint32_t raw_value = 0;
  uint32_t mapping_selector = 0;
  uint32_t slb_half = 0;
  bool little_endian = false;
};

UserModeControlSelection DecodeUserModeControlSelection(
    uint32_t user_mode_control);

// Why an enter was rejected (internal; guest statuses are in
// UserModeBeginEnterResult).
enum class UserModeEnterAdapterDiagnostic {
  kNone,
  kThreadContextMismatch,
  kContextSnapshotFailed,
  kMappingUnavailable,
  kManagerRejected,
};

struct UserModeEnterFromGuestContextResult {
  UserModeEnterAdapterDiagnostic diagnostic =
      UserModeEnterAdapterDiagnostic::kNone;
  UserModeContextSnapshotResult context_snapshot;
  // Set only by BeginUserModeEnterFromGuestControl.
  std::optional<UserModeControlSelection> control_selection;
  std::optional<UserModeMappingSnapshot> resolved_mapping;
  UserModeBeginEnterResult begin;
  bool context_snapshot_attempted = false;
  bool ppc_context_applied = false;

  explicit operator bool() const {
    return diagnostic == UserModeEnterAdapterDiagnostic::kNone &&
           context_snapshot_attempted && static_cast<bool>(context_snapshot) &&
           static_cast<bool>(begin) && ppc_context_applied;
  }
};

enum class UserModeLeaveAdapterDiagnostic {
  kNone,
  kThreadContextMismatch,
  kManagerRejected,
};

struct UserModeLeaveToPpcContextResult {
  UserModeLeaveAdapterDiagnostic diagnostic =
      UserModeLeaveAdapterDiagnostic::kNone;
  UserModeLeaveResult leave;
  bool ppc_context_applied = false;

  explicit operator bool() const {
    return diagnostic == UserModeLeaveAdapterDiagnostic::kNone &&
           static_cast<bool>(leave) && ppc_context_applied;
  }
};

// Capture and restore the integer state in UserModeOuterIntegerState;
// floating-point and vector state are untouched.
UserModeOuterIntegerState CaptureUserModeOuterIntegerState(
    const cpu::ppc::PPCContext& context);
void ApplyUserModeOuterIntegerState(const UserModeOuterIntegerState& state,
                                    cpu::ppc::PPCContext& context);

// Snapshots the guest context, has the manager claim a frame for
// current_thread_id (which must equal PPCContext::thread_id), and applies the
// entry state once every fallible step has succeeded; on failure the
// PPCContext and the frames are unchanged. Takes a host mapping handle; guest
// requests go through BeginUserModeEnterFromGuestControl.
UserModeEnterFromGuestContextResult BeginUserModeEnterFromGuestContext(
    Memory& memory, UserModeManager& manager, uint32_t current_thread_id,
    UserModeMappingHandle mapping, uint32_t context_virtual_address,
    uint32_t handler_address, uint32_t save_area_end, uint32_t companion_state,
    cpu::ppc::PPCContext& ppc_context);

// Snapshots the guest context, decodes its UserModeControl word, resolves the
// Create mapping bit 1 selects, then enters as above. Bits 0 and 2 are
// reported, not applied.
UserModeEnterFromGuestContextResult BeginUserModeEnterFromGuestControl(
    Memory& memory, UserModeManager& manager, uint32_t current_thread_id,
    uint32_t context_virtual_address, uint32_t handler_address,
    uint32_t save_area_end, uint32_t companion_state,
    cpu::ppc::PPCContext& ppc_context);

// Pops current_thread_id's frame and restores its outer integer state; the
// caller chooses where the thread continues.
UserModeLeaveToPpcContextResult RestoreUserModeLeaveToPpcContext(
    UserModeManager& manager, uint32_t current_thread_id,
    cpu::ppc::PPCContext& ppc_context);

std::string_view UserModeEnterAdapterDiagnosticToString(
    UserModeEnterAdapterDiagnostic diagnostic);
std::string_view UserModeLeaveAdapterDiagnosticToString(
    UserModeLeaveAdapterDiagnostic diagnostic);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_ENTER_H_

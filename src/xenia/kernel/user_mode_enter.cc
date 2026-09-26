/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_enter.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/memory.h"

namespace xe::kernel {

UserModeControlSelection DecodeUserModeControlSelection(
    uint32_t user_mode_control) {
  return UserModeControlSelection{
      user_mode_control, (user_mode_control >> 1) & 1, user_mode_control & 1,
      ((user_mode_control >> 2) & 1) != 0};
}

UserModeOuterIntegerState CaptureUserModeOuterIntegerState(
    const cpu::ppc::PPCContext& context) {
  UserModeOuterIntegerState state;
  std::copy(std::begin(context.r), std::end(context.r), state.gpr.begin());
  state.link_register = context.lr;
  state.count_register = context.ctr;
  state.machine_state_register = context.msr;
  state.condition_register = static_cast<uint32_t>(context.cr());
  state.xer_ca = context.xer_ca;
  state.xer_ov = context.xer_ov;
  state.xer_so = context.xer_so;
  return state;
}

void ApplyUserModeOuterIntegerState(const UserModeOuterIntegerState& state,
                                    cpu::ppc::PPCContext& context) {
  std::copy(state.gpr.begin(), state.gpr.end(), std::begin(context.r));
  context.lr = state.link_register;
  context.ctr = state.count_register;
  context.msr = state.machine_state_register;
  context.set_cr(state.condition_register);
  context.xer_ca = state.xer_ca;
  context.xer_ov = state.xer_ov;
  context.xer_so = state.xer_so;
}

namespace {

UserModeEnterFromGuestContextResult ApplyPreparedUserModeEnter(
    UserModeEnterFromGuestContextResult result,
    cpu::ppc::PPCContext& ppc_context) {
  if (!result.begin) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kManagerRejected;
    return result;
  }

  // Last and infallible, so a rejection above leaves the CPU state untouched.
  ApplyUserModeIntegerContext(result.begin.prepared_entry->context,
                              ppc_context);
  result.ppc_context_applied = true;
  return result;
}

UserModeEnterFromGuestContextResult BeginUserModeEnterFromResolvedSnapshot(
    UserModeManager& manager, uint32_t current_thread_id,
    UserModeMappingHandle mapping,
    UserModeContextSnapshotResult context_snapshot, uint32_t handler_address,
    uint32_t save_area_end, uint32_t companion_state,
    cpu::ppc::PPCContext& ppc_context) {
  UserModeEnterFromGuestContextResult result;
  result.context_snapshot_attempted = true;
  result.context_snapshot = std::move(context_snapshot);
  if (!result.context_snapshot) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kContextSnapshotFailed;
    return result;
  }

  UserModeEnterRequest request;
  request.thread_id = current_thread_id;
  request.mapping = mapping;
  request.context_virtual_address =
      result.context_snapshot.context_virtual_address;
  request.entry_context = result.context_snapshot.context;
  request.handler_address = handler_address;
  request.save_area_end = save_area_end;
  request.companion_state = companion_state;
  request.outer_state = CaptureUserModeOuterIntegerState(ppc_context);

  result.begin = manager.BeginEnter(request);
  return ApplyPreparedUserModeEnter(std::move(result), ppc_context);
}

}  // namespace

UserModeEnterFromGuestContextResult BeginUserModeEnterFromGuestContext(
    Memory& memory, UserModeManager& manager, uint32_t current_thread_id,
    UserModeMappingHandle mapping, uint32_t context_virtual_address,
    uint32_t handler_address, uint32_t save_area_end, uint32_t companion_state,
    cpu::ppc::PPCContext& ppc_context) {
  UserModeEnterFromGuestContextResult result;
  if (ppc_context.thread_id != current_thread_id) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kThreadContextMismatch;
    return result;
  }

  return BeginUserModeEnterFromResolvedSnapshot(
      manager, current_thread_id, mapping,
      ResolveUserModeContextSnapshot(memory, context_virtual_address),
      handler_address, save_area_end, companion_state, ppc_context);
}

UserModeEnterFromGuestContextResult BeginUserModeEnterFromGuestControl(
    Memory& memory, UserModeManager& manager, uint32_t current_thread_id,
    uint32_t context_virtual_address, uint32_t handler_address,
    uint32_t save_area_end, uint32_t companion_state,
    cpu::ppc::PPCContext& ppc_context) {
  UserModeEnterFromGuestContextResult result;
  if (ppc_context.thread_id != current_thread_id) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kThreadContextMismatch;
    return result;
  }

  result.context_snapshot_attempted = true;
  result.context_snapshot =
      ResolveUserModeContextSnapshot(memory, context_virtual_address);
  if (!result.context_snapshot) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kContextSnapshotFailed;
    return result;
  }

  result.control_selection = DecodeUserModeControlSelection(
      result.context_snapshot.context.user_mode_control);

  UserModeEnterBySelectorRequest request;
  request.thread_id = current_thread_id;
  request.mapping_selector = result.control_selection->mapping_selector;
  request.context_virtual_address =
      result.context_snapshot.context_virtual_address;
  request.entry_context = result.context_snapshot.context;
  request.handler_address = handler_address;
  request.save_area_end = save_area_end;
  request.companion_state = companion_state;
  request.outer_state = CaptureUserModeOuterIntegerState(ppc_context);

  UserModeBeginEnterBySelectorResult selected =
      manager.BeginEnterBySelector(request);
  result.begin = std::move(selected.begin);
  result.resolved_mapping = std::move(selected.resolved_mapping);
  if (!result.begin &&
      result.begin.diagnostic ==
          UserModeManagerDiagnostic::kInvalidMappingHandle &&
      !result.begin.guest_status.has_value() &&
      !result.resolved_mapping.has_value()) {
    result.diagnostic = UserModeEnterAdapterDiagnostic::kMappingUnavailable;
    return result;
  }
  return ApplyPreparedUserModeEnter(std::move(result), ppc_context);
}

UserModeLeaveToPpcContextResult RestoreUserModeLeaveToPpcContext(
    UserModeManager& manager, uint32_t current_thread_id,
    cpu::ppc::PPCContext& ppc_context) {
  UserModeLeaveToPpcContextResult result;
  if (ppc_context.thread_id != current_thread_id) {
    result.diagnostic = UserModeLeaveAdapterDiagnostic::kThreadContextMismatch;
    return result;
  }

  result.leave = manager.Leave(current_thread_id);
  if (!result.leave) {
    result.diagnostic = UserModeLeaveAdapterDiagnostic::kManagerRejected;
    return result;
  }

  ApplyUserModeOuterIntegerState(*result.leave.outer_state, ppc_context);
  result.ppc_context_applied = true;
  return result;
}

std::string_view UserModeEnterAdapterDiagnosticToString(
    UserModeEnterAdapterDiagnostic diagnostic) {
  switch (diagnostic) {
    case UserModeEnterAdapterDiagnostic::kNone:
      return "none";
    case UserModeEnterAdapterDiagnostic::kThreadContextMismatch:
      return "the supplied PPC context does not belong to the current thread";
    case UserModeEnterAdapterDiagnostic::kContextSnapshotFailed:
      return "the guest user-mode context could not be snapshotted";
    case UserModeEnterAdapterDiagnostic::kMappingUnavailable:
      return "the UserModeControl-selected mapping is unavailable";
    case UserModeEnterAdapterDiagnostic::kManagerRejected:
      return "the user-mode manager rejected the entry";
  }
  return "unknown user-mode Enter adapter diagnostic";
}

std::string_view UserModeLeaveAdapterDiagnosticToString(
    UserModeLeaveAdapterDiagnostic diagnostic) {
  switch (diagnostic) {
    case UserModeLeaveAdapterDiagnostic::kNone:
      return "none";
    case UserModeLeaveAdapterDiagnostic::kThreadContextMismatch:
      return "the supplied PPC context does not belong to the current thread";
    case UserModeLeaveAdapterDiagnostic::kManagerRejected:
      return "the user-mode manager rejected the leave";
  }
  return "unknown user-mode Leave adapter diagnostic";
}

}  // namespace xe::kernel

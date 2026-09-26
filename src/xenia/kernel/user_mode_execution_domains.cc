/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_execution_domains.h"

#include "xenia/cpu/translation_domain_registry.h"

namespace xe::kernel {

UserModeExecutionDomainPlanner::UserModeExecutionDomainPlanner(
    const cpu::TranslationDomainRegistry& registry,
    UserModeExecutionDomainPlannerMode mode)
    : registry_(registry), mode_(mode) {}

bool UserModeExecutionDomainPlanner::IsValidMappingHandle(
    UserModeMappingHandle mapping) {
  return mapping.selector < 2 && mapping.generation != 0;
}

bool UserModeExecutionDomainPlanner::IsValidDomainSet(
    const UserModeExecutionDomainKeys& domains) {
  std::array<cpu::TranslationDomainKey, 4> flattened{};
  size_t flattened_index = 0;
  for (const auto& endian_domains : domains) {
    for (const cpu::TranslationDomainKey& domain : endian_domains) {
      if (domain == cpu::TranslationDomainKey::DefaultDirect() ||
          domain.memory_access_mode() != cpu::MemoryAccessMode::kChecked) {
        return false;
      }
      flattened[flattened_index++] = domain;
    }
  }

  for (size_t left = 0; left < flattened.size(); ++left) {
    for (size_t right = left + 1; right < flattened.size(); ++right) {
      if (flattened[left] == flattened[right]) {
        return false;
      }
    }
  }
  return true;
}

bool UserModeExecutionDomainPlanner::AreDomainsActive(
    const UserModeExecutionDomainKeys& domains) const {
  for (const auto& endian_domains : domains) {
    for (const cpu::TranslationDomainKey& domain : endian_domains) {
      if (!registry_.Lookup(domain)) {
        return false;
      }
    }
  }
  return true;
}

UserModeExecutionDomainDiagnostic UserModeExecutionDomainPlanner::BindMapping(
    UserModeMappingHandle mapping, const UserModeExecutionDomainKeys& domains) {
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    return UserModeExecutionDomainDiagnostic::kDisabled;
  }
  if (!IsValidMappingHandle(mapping)) {
    return UserModeExecutionDomainDiagnostic::kInvalidMappingHandle;
  }
  if (!IsValidDomainSet(domains)) {
    return UserModeExecutionDomainDiagnostic::kInvalidDomainSet;
  }

  // Check the registry outside mutex_; plans check again on their own
  // snapshot, since the domains may retire meanwhile.
  if (!AreDomainsActive(domains)) {
    return UserModeExecutionDomainDiagnostic::kDomainNotActive;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<UserModeExecutionDomainBinding>& binding =
      bindings_[mapping.selector];
  if (binding.has_value()) {
    if (binding->mapping == mapping && binding->domains == domains) {
      return UserModeExecutionDomainDiagnostic::kNone;
    }
    return UserModeExecutionDomainDiagnostic::kBindingConflict;
  }
  if (binding_tombstones_[mapping.selector]) {
    return UserModeExecutionDomainDiagnostic::kBindingConflict;
  }
  binding = UserModeExecutionDomainBinding{mapping, domains};
  return UserModeExecutionDomainDiagnostic::kNone;
}

UserModeExecutionDomainDiagnostic UserModeExecutionDomainPlanner::UnbindMapping(
    UserModeMappingHandle mapping) {
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    return UserModeExecutionDomainDiagnostic::kDisabled;
  }
  if (!IsValidMappingHandle(mapping)) {
    return UserModeExecutionDomainDiagnostic::kInvalidMappingHandle;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<UserModeExecutionDomainBinding>& binding =
      bindings_[mapping.selector];
  if (!binding.has_value() || binding->mapping != mapping) {
    return UserModeExecutionDomainDiagnostic::kBindingNotFound;
  }
  binding.reset();
  binding_tombstones_[mapping.selector] = true;
  return UserModeExecutionDomainDiagnostic::kNone;
}

UserModeExecutionDomainDiagnostic UserModeExecutionDomainPlanner::Reset() {
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    return UserModeExecutionDomainDiagnostic::kDisabled;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t selector = 0; selector < bindings_.size(); ++selector) {
    bindings_[selector].reset();
    binding_tombstones_[selector] = true;
  }
  return UserModeExecutionDomainDiagnostic::kNone;
}

size_t UserModeExecutionDomainPlanner::binding_count() const {
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& binding : bindings_) {
    count += binding.has_value() ? 1 : 0;
  }
  return count;
}

UserModeExecutionDomainDiagnostic
UserModeExecutionDomainPlanner::SnapshotActiveBinding(
    UserModeMappingHandle mapping,
    UserModeExecutionDomainBinding* binding_out) const {
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    return UserModeExecutionDomainDiagnostic::kDisabled;
  }
  if (!IsValidMappingHandle(mapping) || !binding_out) {
    return UserModeExecutionDomainDiagnostic::kInvalidMappingHandle;
  }

  UserModeExecutionDomainBinding binding;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<UserModeExecutionDomainBinding>& stored_binding =
        bindings_[mapping.selector];
    if (!stored_binding.has_value() || stored_binding->mapping != mapping) {
      return UserModeExecutionDomainDiagnostic::kBindingNotFound;
    }
    binding = *stored_binding;
  }

  // Check the four keys outside mutex_; keep only the selected key.
  if (!AreDomainsActive(binding.domains)) {
    return UserModeExecutionDomainDiagnostic::kDomainNotActive;
  }

  *binding_out = binding;
  return UserModeExecutionDomainDiagnostic::kNone;
}

UserModeExecutionDomainDiagnostic
UserModeExecutionDomainPlanner::SelectDomainForControl(
    const UserModeExecutionDomainBinding& binding, uint32_t user_mode_control,
    cpu::TranslationDomainKey* translation_domain_key_out,
    uint32_t* slb_half_out, bool* little_endian_out) {
  if (!translation_domain_key_out || !slb_half_out || !little_endian_out) {
    return UserModeExecutionDomainDiagnostic::kInvalidDomainSet;
  }

  const UserModeControlSelection selection =
      DecodeUserModeControlSelection(user_mode_control);
  if (selection.mapping_selector != binding.mapping.selector) {
    return UserModeExecutionDomainDiagnostic::kInvalidMappingHandle;
  }

  *slb_half_out = selection.slb_half;
  *little_endian_out = selection.little_endian;
  *translation_domain_key_out =
      binding.domains[selection.little_endian ? 1 : 0][selection.slb_half];
  return UserModeExecutionDomainDiagnostic::kNone;
}

UserModeExecutionDomainPlan UserModeExecutionDomainPlanner::PlanEnter(
    const UserModeEnterFromGuestContextResult& enter_result) const {
  UserModeExecutionDomainPlan plan;
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    plan.diagnostic = UserModeExecutionDomainDiagnostic::kDisabled;
    return plan;
  }
  if (!enter_result || !enter_result.control_selection.has_value() ||
      !enter_result.resolved_mapping.has_value() ||
      !enter_result.begin.prepared_entry.has_value()) {
    plan.diagnostic = UserModeExecutionDomainDiagnostic::kInvalidEnterResult;
    return plan;
  }

  const UserModeControlSelection& supplied_selection =
      *enter_result.control_selection;
  const UserModePreparedEntry& prepared_entry =
      *enter_result.begin.prepared_entry;
  const UserModeMappingSnapshot& mapping_snapshot =
      *enter_result.resolved_mapping;
  const UserModeControlSelection decoded_selection =
      DecodeUserModeControlSelection(supplied_selection.raw_value);
  if (decoded_selection.mapping_selector !=
          supplied_selection.mapping_selector ||
      decoded_selection.slb_half != supplied_selection.slb_half ||
      decoded_selection.little_endian != supplied_selection.little_endian ||
      mapping_snapshot.handle != prepared_entry.mapping ||
      mapping_snapshot.handle.selector != supplied_selection.mapping_selector ||
      prepared_entry.context.user_mode_control !=
          supplied_selection.raw_value ||
      enter_result.context_snapshot.context.user_mode_control !=
          supplied_selection.raw_value) {
    plan.diagnostic = UserModeExecutionDomainDiagnostic::kInvalidEnterResult;
    return plan;
  }

  UserModeExecutionDomainBinding binding;
  plan.diagnostic = SnapshotActiveBinding(mapping_snapshot.handle, &binding);
  if (plan.diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
    return plan;
  }
  plan.diagnostic = SelectDomainForControl(
      binding, supplied_selection.raw_value, &plan.target_domain_key,
      &plan.user_slb_half, &plan.user_little_endian);
  if (plan.diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
    return plan;
  }

  plan.transition = UserModeExecutionDomainTransition::kEnter;
  plan.mapping = mapping_snapshot.handle;
  plan.expected_source_domain_key = cpu::TranslationDomainKey::DefaultDirect();
  plan.resume_user_domain_key = plan.target_domain_key;
  plan.entry_address = prepared_entry.context.entry_address;
  plan.handler_address = prepared_entry.handler_address;
  plan.previous_user_mode_control = supplied_selection.raw_value;
  plan.target_user_mode_control = supplied_selection.raw_value;
  return plan;
}

UserModeExecutionDomainPlan
UserModeExecutionDomainPlanner::PlanProblemStateSystemCall(
    const UserModeProblemStateSystemCallResult& system_call_result) const {
  UserModeExecutionDomainPlan plan;
  if (mode_ == UserModeExecutionDomainPlannerMode::kDisabled) {
    plan.diagnostic = UserModeExecutionDomainDiagnostic::kDisabled;
    return plan;
  }
  if (!system_call_result ||
      !IsValidMappingHandle(system_call_result.mapping) ||
      system_call_result.frame_revision == 0) {
    plan.diagnostic =
        UserModeExecutionDomainDiagnostic::kInvalidSystemCallResult;
    return plan;
  }
  if (!system_call_result.mapping_is_current) {
    plan.diagnostic = UserModeExecutionDomainDiagnostic::kMappingNotCurrent;
    return plan;
  }

  const bool switches_slb_half = system_call_result.number <= 1;
  if (switches_slb_half) {
    const uint32_t expected_control =
        (system_call_result.previous_user_mode_control & ~uint32_t{1}) |
        static_cast<uint32_t>(system_call_result.number);
    if (system_call_result.action !=
            UserModeProblemStateSystemCallAction::kSwitchSlbHalf ||
        system_call_result.current_user_mode_control != expected_control) {
      plan.diagnostic =
          UserModeExecutionDomainDiagnostic::kInvalidSystemCallResult;
      return plan;
    }
  } else {
    if (system_call_result.action !=
            UserModeProblemStateSystemCallAction::kDeliverToHandler ||
        system_call_result.current_user_mode_control !=
            system_call_result.previous_user_mode_control ||
        system_call_result.handler_address == 0) {
      plan.diagnostic =
          UserModeExecutionDomainDiagnostic::kInvalidSystemCallResult;
      return plan;
    }
  }

  // Snapshot the four identities once so the old and new views cannot be
  // selected from different same-handle publications.
  UserModeExecutionDomainBinding binding;
  plan.diagnostic = SnapshotActiveBinding(system_call_result.mapping, &binding);
  if (plan.diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
    return plan;
  }

  cpu::TranslationDomainKey previous_user_domain_key;
  uint32_t previous_slb_half = 0;
  bool previous_little_endian = false;
  plan.diagnostic = SelectDomainForControl(
      binding, system_call_result.previous_user_mode_control,
      &previous_user_domain_key, &previous_slb_half, &previous_little_endian);
  if (plan.diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
    return plan;
  }

  cpu::TranslationDomainKey current_user_domain_key;
  plan.diagnostic = SelectDomainForControl(
      binding, system_call_result.current_user_mode_control,
      &current_user_domain_key, &plan.user_slb_half, &plan.user_little_endian);
  if (plan.diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
    return plan;
  }

  plan.expected_source_domain_key = previous_user_domain_key;
  plan.expected_source_user_slb_half = previous_slb_half;
  plan.expected_source_user_little_endian = previous_little_endian;
  plan.resume_user_domain_key = current_user_domain_key;
  if (switches_slb_half) {
    plan.transition = UserModeExecutionDomainTransition::kSwitchSlbHalf;
    plan.target_domain_key = current_user_domain_key;
    plan.clear_r0_required = true;
  } else {
    plan.transition = UserModeExecutionDomainTransition::kDeliverToHandler;
    plan.target_domain_key = cpu::TranslationDomainKey::DefaultDirect();
  }

  plan.mapping = system_call_result.mapping;
  plan.handler_address = system_call_result.handler_address;
  plan.system_call_number = system_call_result.number;
  plan.frame_revision = system_call_result.frame_revision;
  plan.previous_user_mode_control =
      system_call_result.previous_user_mode_control;
  plan.target_user_mode_control = system_call_result.current_user_mode_control;
  return plan;
}

}  // namespace xe::kernel

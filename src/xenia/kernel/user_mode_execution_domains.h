/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_EXECUTION_DOMAINS_H_
#define XENIA_KERNEL_USER_MODE_EXECUTION_DOMAINS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "xenia/cpu/translation_domain.h"
#include "xenia/kernel/user_mode_enter.h"
#include "xenia/kernel/user_mode_manager.h"

namespace xe::cpu {
class TranslationDomainRegistry;
}  // namespace xe::cpu

namespace xe::kernel {

// The planner selects the checked domain a mapping and control word run in;
// UserModeGuestCodeDomainCoordinator authorizes execution.
enum class UserModeExecutionDomainPlannerMode {
  kDisabled,
  kSyntheticPlanOnly,
};

enum class UserModeExecutionDomainDiagnostic {
  kNone,
  kDisabled,
  kInvalidMappingHandle,
  kInvalidDomainSet,
  kDomainNotActive,
  kBindingNotFound,
  kBindingConflict,
  kInvalidEnterResult,
  kInvalidSystemCallResult,
  kMappingNotCurrent,
};

// Indexed by the target MSR LE bit, then the SLB-half bit. The four domains
// are checked and distinct, so translations of different views never alias.
using UserModeExecutionDomainKeys =
    std::array<std::array<cpu::TranslationDomainKey, 2>, 2>;

struct UserModeExecutionDomainBinding {
  UserModeMappingHandle mapping;
  UserModeExecutionDomainKeys domains;
};

enum class UserModeExecutionDomainTransition {
  kEnter,
  kSwitchSlbHalf,
  kDeliverToHandler,
};

// A domain selection. execution_permitted is always false: the coordinator
// authorizes execution.
struct UserModeExecutionDomainPlan {
  UserModeExecutionDomainDiagnostic diagnostic =
      UserModeExecutionDomainDiagnostic::kNone;
  std::optional<UserModeExecutionDomainTransition> transition;
  UserModeMappingHandle mapping;
  // The domains the switch leaves and enters. A custom callback targets
  // DefaultDirect and keeps the user domain to resume into.
  cpu::TranslationDomainKey expected_source_domain_key =
      cpu::TranslationDomainKey::DefaultDirect();
  cpu::TranslationDomainKey target_domain_key =
      cpu::TranslationDomainKey::DefaultDirect();
  cpu::TranslationDomainKey resume_user_domain_key =
      cpu::TranslationDomainKey::DefaultDirect();

  uint32_t entry_address = 0;
  uint32_t handler_address = 0;
  uint64_t system_call_number = 0;
  uint64_t frame_revision = 0;
  uint32_t previous_user_mode_control = 0;
  uint32_t target_user_mode_control = 0;
  uint32_t expected_source_user_slb_half = 0;
  bool expected_source_user_little_endian = false;
  uint32_t user_slb_half = 0;
  bool user_little_endian = false;
  bool clear_r0_required = false;
  static constexpr bool execution_permitted = false;

  // Whether the selection is consistent; it grants no permission to run it.
  bool IsStructurallyValid() const {
    if (diagnostic != UserModeExecutionDomainDiagnostic::kNone ||
        !transition.has_value() || !static_cast<bool>(mapping) ||
        resume_user_domain_key == cpu::TranslationDomainKey::DefaultDirect()) {
      return false;
    }
    switch (*transition) {
      case UserModeExecutionDomainTransition::kEnter:
        return expected_source_domain_key ==
                   cpu::TranslationDomainKey::DefaultDirect() &&
               target_domain_key == resume_user_domain_key &&
               frame_revision == 0;
      case UserModeExecutionDomainTransition::kSwitchSlbHalf:
        return expected_source_domain_key !=
                   cpu::TranslationDomainKey::DefaultDirect() &&
               target_domain_key == resume_user_domain_key &&
               frame_revision != 0;
      case UserModeExecutionDomainTransition::kDeliverToHandler:
        return expected_source_domain_key == resume_user_domain_key &&
               target_domain_key ==
                   cpu::TranslationDomainKey::DefaultDirect() &&
               frame_revision != 0;
    }
    return false;
  }

  explicit operator bool() const = delete;
};

class UserModeExecutionDomainPlanner final {
 public:
  explicit UserModeExecutionDomainPlanner(
      const cpu::TranslationDomainRegistry& registry,
      UserModeExecutionDomainPlannerMode mode =
          UserModeExecutionDomainPlannerMode::kDisabled);

  UserModeExecutionDomainPlanner(const UserModeExecutionDomainPlanner&) =
      delete;
  UserModeExecutionDomainPlanner& operator=(
      const UserModeExecutionDomainPlanner&) = delete;

  UserModeExecutionDomainPlannerMode mode() const { return mode_; }

  // Binds a mapping generation to its four domains, in the planner's registry.
  // Repeating the active binding is a no-op; replacing another fails, and an
  // unbound or reset selector can't be bound again.
  UserModeExecutionDomainDiagnostic BindMapping(
      UserModeMappingHandle mapping,
      const UserModeExecutionDomainKeys& domains);

  // Removes only the binding of that exact generation; the selector can't be
  // bound again.
  UserModeExecutionDomainDiagnostic UnbindMapping(
      UserModeMappingHandle mapping);
  UserModeExecutionDomainDiagnostic Reset();
  size_t binding_count() const;

  // Selects the domain a guest-control Enter runs in.
  UserModeExecutionDomainPlan PlanEnter(
      const UserModeEnterFromGuestContextResult& enter_result) const;

  // Selects the domains of a problem-state system call, for the mapping
  // generation and frame revision the manager captured.
  UserModeExecutionDomainPlan PlanProblemStateSystemCall(
      const UserModeProblemStateSystemCallResult& system_call_result) const;

 private:
  static bool IsValidMappingHandle(UserModeMappingHandle mapping);
  static bool IsValidDomainSet(const UserModeExecutionDomainKeys& domains);
  bool AreDomainsActive(const UserModeExecutionDomainKeys& domains) const;

  UserModeExecutionDomainDiagnostic SnapshotActiveBinding(
      UserModeMappingHandle mapping,
      UserModeExecutionDomainBinding* binding_out) const;
  static UserModeExecutionDomainDiagnostic SelectDomainForControl(
      const UserModeExecutionDomainBinding& binding, uint32_t user_mode_control,
      cpu::TranslationDomainKey* translation_domain_key_out,
      uint32_t* slb_half_out, bool* little_endian_out);

  const cpu::TranslationDomainRegistry& registry_;
  const UserModeExecutionDomainPlannerMode mode_;
  mutable std::mutex mutex_;
  std::array<std::optional<UserModeExecutionDomainBinding>, 2> bindings_{};
  std::array<bool, 2> binding_tombstones_{};
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_EXECUTION_DOMAINS_H_

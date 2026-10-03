/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_GUEST_CODE_DOMAINS_H_
#define XENIA_KERNEL_USER_MODE_GUEST_CODE_DOMAINS_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "xenia/cpu/processor.h"
#include "xenia/kernel/user_mode_execution_domains.h"
#include "xenia/kernel/user_mode_manager.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::cpu {
class Module;
class TranslationDomainRegistry;
}  // namespace xe::cpu

namespace xe::kernel {

class UserModeGuestCodeDomainCoordinatorState;
class UserModeGuestCodeDomainTransitionPermit;

enum class UserModeGuestCodeDomainRegistrationDiagnostic {
  kNone,
  kInvalidMappingSnapshot,
  kMappingNotCurrent,
  kRegistrationFailed,
};

struct UserModeGuestCodeDomainRegistrationResult {
  UserModeGuestCodeDomainRegistrationDiagnostic diagnostic =
      UserModeGuestCodeDomainRegistrationDiagnostic::kNone;
  std::optional<UserModeExecutionDomainKeys> domains;

  // Registration alone doesn't authorize execution.
  static constexpr bool execution_permitted = false;

  explicit operator bool() const {
    return diagnostic == UserModeGuestCodeDomainRegistrationDiagnostic::kNone &&
           domains.has_value();
  }
};

enum class UserModeGuestCodeDomainCoordinatorMode {
  kDisabled,
  kSyntheticStateOnly,
  kExecutionEnabled,
};

enum class UserModeGuestCodeDomainCoordinatorDiagnostic {
  kNone,
  kDisabled,
  kInvalidMappingSnapshot,
  kMappingNotCurrent,
  kDuplicateMapping,
  kSelectorConflict,
  kDomainRegistrationFailed,
  kPlannerBindingFailed,
  kLeaseCreationFailed,
  kRetirementListenerAttachmentFailed,
  kMappingRetiredDuringRegistration,
  kProcessorUnavailable,
  kDispatcherUnavailable,
  kTransitionUnavailable,
};

class UserModeGuestCodeDomainLease final
    : public UserModeMappingRetirementListener {
 public:
  ~UserModeGuestCodeDomainLease() override;

  UserModeGuestCodeDomainLease(const UserModeGuestCodeDomainLease&) = delete;
  UserModeGuestCodeDomainLease& operator=(const UserModeGuestCodeDomainLease&) =
      delete;

  UserModeMappingHandle mapping() const { return mapping_; }
  const UserModeExecutionDomainKeys& domains() const { return domains_; }
  bool is_active() const { return active_.load(std::memory_order_acquire); }

 private:
  friend class UserModeGuestCodeDomainCoordinator;
  friend class UserModeGuestCodeDomainCoordinatorState;
  friend class UserModeGuestCodeDomainTransitionPermit;

  UserModeGuestCodeDomainLease(
      cpu::TranslationDomainRegistry& registry, UserModeMappingHandle mapping,
      std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
      UserModeExecutionDomainKeys domains,
      std::unique_ptr<UserModeExecutionDomainPlanner> planner,
      cpu::Processor* processor, std::array<std::string, 4> mapped_module_names,
      std::weak_ptr<UserModeGuestCodeDomainCoordinatorState> owner_state);

  bool TryAcquireTransition() noexcept;
  void ReleaseTransition() noexcept;
  bool TryBeginFlush() noexcept;
  void EndFlush() noexcept;
  bool CanBeginExecution() const noexcept;
  bool ContainsDomain(cpu::TranslationDomainKey domain) const noexcept;
  void OnUserModeMappingRetired(
      UserModeMappingHandle mapping) noexcept override;
  void RetireResources() noexcept;

  cpu::TranslationDomainRegistry& registry_;
  const UserModeMappingHandle mapping_;
  const std::shared_ptr<const UserModeMappingGenerationToken> generation_token_;
  const UserModeExecutionDomainKeys domains_;
  const std::unique_ptr<UserModeExecutionDomainPlanner> planner_;
  cpu::Processor* const processor_;
  const std::array<std::string, 4> mapped_module_names_;
  const std::weak_ptr<UserModeGuestCodeDomainCoordinatorState> owner_state_;
  std::once_flag retirement_once_;
  std::atomic<bool> active_{true};
  std::atomic<bool> retiring_{false};
  std::atomic<bool> flushing_{false};
  mutable std::mutex transition_mutex_;
  std::condition_variable transition_condition_;
  size_t active_transition_count_ = 0;
};

struct UserModeGuestCodeDomainCoordinatorResult {
  UserModeGuestCodeDomainCoordinatorDiagnostic diagnostic =
      UserModeGuestCodeDomainCoordinatorDiagnostic::kNone;
  std::optional<UserModeGuestCodeDomainRegistrationDiagnostic>
      domain_registration_diagnostic;
  std::optional<UserModeExecutionDomainDiagnostic> planner_diagnostic;
  std::optional<UserModeMappingRetirementListenerAttachmentDiagnostic>
      listener_attachment_diagnostic;
  std::shared_ptr<const UserModeGuestCodeDomainLease> lease;

  bool execution_permitted = false;

  // Whether the lease committed; lease->is_active() says whether it is live.
  explicit operator bool() const {
    return diagnostic == UserModeGuestCodeDomainCoordinatorDiagnostic::kNone &&
           lease;
  }
};

// A domain switch and its permit, which XThread carries across the unwind to
// Processor::CommitThreadTranslationDomain.
struct UserModeGuestCodeDomainTransitionResult {
  UserModeGuestCodeDomainCoordinatorDiagnostic diagnostic =
      UserModeGuestCodeDomainCoordinatorDiagnostic::kNone;
  UserModeMappingHandle mapping;
  cpu::TranslationDomainKey expected_source_domain_key =
      cpu::TranslationDomainKey::DefaultDirect();
  cpu::TranslationDomainKey target_domain_key =
      cpu::TranslationDomainKey::DefaultDirect();
  std::shared_ptr<const cpu::TranslationDomainTransitionPermit> permit;
  bool execution_permitted = false;

  explicit operator bool() const {
    return diagnostic == UserModeGuestCodeDomainCoordinatorDiagnostic::kNone &&
           static_cast<bool>(mapping) && permit && execution_permitted;
  }
};

enum class UserModeGuestCodeDomainFlushDiagnostic {
  kNone,
  kDisabled,
  kMappingUnavailable,
  kProcessorUnavailable,
  kLiveTargetThreads,
  kInvalidationFailed,
};

struct UserModeGuestCodeDomainFlushResult {
  UserModeGuestCodeDomainFlushDiagnostic diagnostic =
      UserModeGuestCodeDomainFlushDiagnostic::kNone;
  std::array<cpu::GuestCodeInvalidationResult, 4> domain_results{};
  size_t invalidated_domain_count = 0;

  explicit operator bool() const {
    return diagnostic == UserModeGuestCodeDomainFlushDiagnostic::kNone &&
           invalidated_domain_count == domain_results.size();
  }
};

// Owns one checked-domain lease per mapping generation: its four checked
// domains with their planner, and in execution mode their checked accessors and
// modules and the Processor's authorization and fault dispatch. The registry
// must outlive the coordinator, and Memory the registry.
class UserModeGuestCodeDomainCoordinator final
    : public cpu::CheckedGuestMemoryDispatcher {
 public:
  UserModeGuestCodeDomainCoordinator(
      Memory& memory, cpu::TranslationDomainRegistry& registry,
      UserModeGuestCodeDomainCoordinatorMode mode =
          UserModeGuestCodeDomainCoordinatorMode::kDisabled);
  ~UserModeGuestCodeDomainCoordinator();

  UserModeGuestCodeDomainCoordinator(
      const UserModeGuestCodeDomainCoordinator&) = delete;
  UserModeGuestCodeDomainCoordinator& operator=(
      const UserModeGuestCodeDomainCoordinator&) = delete;

  UserModeGuestCodeDomainCoordinatorMode mode() const { return mode_; }

  bool InstallCheckedGuestMemoryFaultHandler(
      cpu::CheckedGuestMemoryFaultHandler* handler);
  bool UninstallCheckedGuestMemoryFaultHandler(
      cpu::CheckedGuestMemoryFaultHandler* handler);
  cpu::CheckedGuestMemoryFaultHandler* checked_guest_memory_fault_handler()
      const {
    return checked_guest_memory_fault_handler_.load(std::memory_order_acquire);
  }

  UserModeGuestCodeDomainCoordinatorResult RegisterMapping(
      const UserModeMappingSnapshot& mapping_snapshot);
  std::shared_ptr<const UserModeGuestCodeDomainLease> FindLease(
      UserModeMappingHandle mapping) const;
  UserModeGuestCodeDomainTransitionResult AcquireTransition(
      const UserModeExecutionDomainPlan& plan) const;
  UserModeGuestCodeDomainTransitionResult AcquireTransition(
      UserModeMappingHandle mapping,
      cpu::TranslationDomainKey expected_source_domain_key,
      cpu::TranslationDomainKey target_domain_key) const;
  // Retires every translation of the mapping's domains: closes new
  // transitions, checks that no live thread is in the four domains, then
  // flushes them. The caller must have left any checked frame.
  UserModeGuestCodeDomainFlushResult InvalidateMappingDomains(
      UserModeMappingHandle mapping);
  // Live TB flush of the mapping's domains while threads run in them: retires
  // the translations whose code moved, probing only flushed_ranges when given.
  // Never closes execution. The caller must hold neither the global nor the
  // resolution lock.
  cpu::GuestCodeInvalidationResult InvalidateMappingDomainsLive(
      UserModeMappingHandle mapping,
      std::optional<std::span<const cpu::FlushedGuestPageRange>>
          flushed_ranges = std::nullopt);
  cpu::GuestCodeInvalidationResult InvalidateAllMappingDomainsLive();
  size_t active_lease_count() const;

  bool IsDomainExecutionAuthorized(
      cpu::TranslationDomainKey translation_domain_key,
      const cpu::TranslationDomainDescriptor& descriptor)
      const noexcept override;
  bool IsDomainTransitionAuthorized(
      cpu::TranslationDomainKey current_domain_key,
      cpu::TranslationDomainKey target_domain_key,
      const cpu::TranslationDomainDescriptor* target_descriptor,
      const cpu::TranslationDomainTransitionPermit& permit)
      const noexcept override;
  [[noreturn]] void HandleGuestMemoryFault(
      cpu::ppc::PPCContext* context,
      cpu::TranslationDomainKey translation_domain_key,
      const cpu::GuestMemoryAccessRequest& request,
      const cpu::GuestMemoryAccessResult& result) override;
  bool DispatchGuestInstructionStorageFault(
      cpu::ppc::PPCContext* context,
      cpu::TranslationDomainKey translation_domain_key,
      const cpu::ppc::GuestInstructionFetchFault& fault) override;

 private:
  Memory& memory_;
  cpu::TranslationDomainRegistry& registry_;
  const UserModeGuestCodeDomainCoordinatorMode mode_;
  const std::shared_ptr<UserModeGuestCodeDomainCoordinatorState> state_;
  cpu::Processor* const processor_;
  const bool processor_dispatcher_installed_;
  std::atomic<cpu::CheckedGuestMemoryFaultHandler*>
      checked_guest_memory_fault_handler_{nullptr};
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_GUEST_CODE_DOMAINS_H_

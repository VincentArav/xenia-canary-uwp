/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_guest_code_domains.h"

#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/mapped_module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/translation_domain_registry.h"
#include "xenia/kernel/user_mode_guest_code_reader.h"
#include "xenia/kernel/user_mode_guest_data_access.h"

namespace xe::kernel {
namespace {

bool LayoutsEqual(const UserModeCreateTimeAddressSpaceLayout& left,
                  const UserModeCreateTimeAddressSpaceLayout& right) {
  if (left.packed_page_table_kind_summary() !=
      right.packed_page_table_kind_summary()) {
    return false;
  }
  for (size_t i = 0; i < left.segments().size(); ++i) {
    const UserModeCreateTimeSegmentLayout& left_segment = left.segments()[i];
    const UserModeCreateTimeSegmentLayout& right_segment = right.segments()[i];
    if (left_segment.raw_create_flags != right_segment.raw_create_flags ||
        left_segment.translation.page_table_kind !=
            right_segment.translation.page_table_kind ||
        left_segment.translation.segment_no_execute !=
            right_segment.translation.segment_no_execute) {
      return false;
    }
  }
  return true;
}

bool IsValidMappingSnapshot(const UserModeMappingSnapshot& snapshot) {
  return snapshot.handle.selector < 2 && snapshot.handle.generation != 0 &&
         snapshot.control_page_physical_address != 0 &&
         (snapshot.control_page_physical_address & 0xFFF) == 0 &&
         snapshot.control_page_physical_address < 0x20000000 &&
         snapshot.create_time_layout.has_value() && snapshot.generation_token &&
         snapshot.generation_token->handle() == snapshot.handle &&
         snapshot.generation_token->control_page_physical_address() ==
             snapshot.control_page_physical_address &&
         snapshot.generation_token->create_time_layout().has_value() &&
         LayoutsEqual(*snapshot.generation_token->create_time_layout(),
                      *snapshot.create_time_layout);
}

void RetireRegisteredDomains(
    cpu::TranslationDomainRegistry& registry,
    const std::array<cpu::TranslationDomainKey, 4>& registered_domains,
    size_t registered_domain_count) {
  for (size_t i = 0; i < registered_domain_count; ++i) {
    registry.Retire(registered_domains[i]);
  }
}

UserModeGuestCodeDomainRegistrationResult
RegisterUserModeGuestCodeDomainsInternal(
    Memory& memory, cpu::TranslationDomainRegistry& registry,
    const UserModeMappingSnapshot& mapping_snapshot,
    UserModeGuestDataStoreMode data_store_mode) {
  UserModeGuestCodeDomainRegistrationResult result;
  if (!IsValidMappingSnapshot(mapping_snapshot)) {
    result.diagnostic =
        UserModeGuestCodeDomainRegistrationDiagnostic::kInvalidMappingSnapshot;
    return result;
  }
  if (!mapping_snapshot.generation_token->is_current()) {
    result.diagnostic =
        UserModeGuestCodeDomainRegistrationDiagnostic::kMappingNotCurrent;
    return result;
  }

  UserModeExecutionDomainKeys domains{};
  std::array<cpu::TranslationDomainKey, 4> registered_domains{};
  size_t registered_domain_count = 0;
  try {
    for (size_t endian_index = 0; endian_index < domains.size();
         ++endian_index) {
      for (size_t key_index = 0; key_index < domains[endian_index].size();
           ++key_index) {
        if (!mapping_snapshot.generation_token->is_current()) {
          RetireRegisteredDomains(registry, registered_domains,
                                  registered_domain_count);
          result.diagnostic =
              UserModeGuestCodeDomainRegistrationDiagnostic::kMappingNotCurrent;
          return result;
        }

        auto reader = std::make_unique<UserModeGuestCodeReader>(
            &memory, mapping_snapshot.generation_token,
            key_index == 0 ? UserModeProblemStateKey::kZero
                           : UserModeProblemStateKey::kOne,
            endian_index != 0);
        auto data_accessor = CreateUserModeGuestMemoryAccessor(
            &memory, mapping_snapshot.generation_token,
            key_index == 0 ? UserModeProblemStateKey::kZero
                           : UserModeProblemStateKey::kOne,
            endian_index != 0, data_store_mode);
        if (!data_accessor) {
          RetireRegisteredDomains(registry, registered_domains,
                                  registered_domain_count);
          result.diagnostic =
              mapping_snapshot.generation_token->is_current()
                  ? UserModeGuestCodeDomainRegistrationDiagnostic::
                        kRegistrationFailed
                  : UserModeGuestCodeDomainRegistrationDiagnostic::
                        kMappingNotCurrent;
          return result;
        }
        const cpu::TranslationDomainDescriptor* const descriptor =
            registry.Register(cpu::MemoryAccessMode::kChecked,
                              std::move(reader), std::move(data_accessor));
        if (!descriptor) {
          RetireRegisteredDomains(registry, registered_domains,
                                  registered_domain_count);
          result.diagnostic =
              mapping_snapshot.generation_token->is_current()
                  ? UserModeGuestCodeDomainRegistrationDiagnostic::
                        kRegistrationFailed
                  : UserModeGuestCodeDomainRegistrationDiagnostic::
                        kMappingNotCurrent;
          return result;
        }

        const cpu::TranslationDomainKey domain_key =
            descriptor->translation_domain_key();
        domains[endian_index][key_index] = domain_key;
        registered_domains[registered_domain_count++] = domain_key;
      }
    }
  } catch (...) {
    // On failure, retire the domains registered so far.
    RetireRegisteredDomains(registry, registered_domains,
                            registered_domain_count);
    result.diagnostic =
        UserModeGuestCodeDomainRegistrationDiagnostic::kRegistrationFailed;
    return result;
  }

  if (!mapping_snapshot.generation_token->is_current()) {
    RetireRegisteredDomains(registry, registered_domains,
                            registered_domain_count);
    result.diagnostic =
        UserModeGuestCodeDomainRegistrationDiagnostic::kMappingNotCurrent;
    return result;
  }

  result.domains = domains;
  return result;
}

void RetireDomainSet(cpu::TranslationDomainRegistry& registry,
                     const UserModeExecutionDomainKeys& domains) noexcept {
  for (const auto& endian_domains : domains) {
    for (const cpu::TranslationDomainKey& domain : endian_domains) {
      registry.Retire(domain);
    }
  }
}

std::string MappedModuleName(cpu::TranslationDomainKey domain) {
  return "__user_mode_mapped_" + std::to_string(domain.id());
}

void RemoveMappedModules(
    cpu::Processor& processor, const UserModeExecutionDomainKeys& domains,
    const std::array<std::string, 4>& module_names) noexcept {
  size_t index = 0;
  for (const auto& endian_domains : domains) {
    for (cpu::TranslationDomainKey domain : endian_domains) {
      if (!module_names[index].empty()) {
        processor.RemoveModule(domain, module_names[index]);
      }
      ++index;
    }
  }
}

bool InstallMappedModules(cpu::Processor& processor,
                          const UserModeExecutionDomainKeys& domains,
                          std::array<std::string, 4>* module_names_out) {
  if (!module_names_out || !processor.backend() ||
      !processor.backend()->SupportsCheckedGuestMemoryAccess()) {
    return false;
  }

  size_t installed_count = 0;
  try {
    for (const auto& endian_domains : domains) {
      for (cpu::TranslationDomainKey domain : endian_domains) {
        std::string name = MappedModuleName(domain);
        if (!processor.AddModule(std::make_unique<cpu::MappedModule>(
                &processor, domain, name))) {
          throw std::bad_alloc();
        }
        (*module_names_out)[installed_count++] = std::move(name);
      }
    }
  } catch (...) {
    size_t domain_index = 0;
    for (const auto& endian_domains : domains) {
      for (cpu::TranslationDomainKey domain : endian_domains) {
        if (domain_index < installed_count) {
          processor.RemoveModule(domain, (*module_names_out)[domain_index]);
        }
        ++domain_index;
      }
    }
    return false;
  }
  return true;
}

bool HasLiveCheckedMemoryCapabilities(
    const cpu::TranslationDomainDescriptor& descriptor) {
  const cpu::GuestMemoryAccessor* accessor = descriptor.guest_memory_accessor();
  if (!accessor ||
      accessor->memory_access_mode() != cpu::MemoryAccessMode::kChecked) {
    return false;
  }
  const cpu::GuestMemoryAccessCapabilities capabilities =
      accessor->capabilities();
  constexpr uint32_t kAllOperandTypes =
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt8) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt16) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt32) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt64) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kFloat32) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kFloat64) |
      cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kVector128);
  return capabilities.live_checked_execution &&
         (capabilities.load_operand_types & kAllOperandTypes) ==
             kAllOperandTypes &&
         (capabilities.store_operand_types & kAllOperandTypes) ==
             kAllOperandTypes &&
         capabilities.vector_left_right_load &&
         capabilities.vector_left_right_store && capabilities.memset &&
         capabilities.atomic_compare_exchange_32 &&
         capabilities.atomic_compare_exchange_64 &&
         capabilities.reservation_32 && capabilities.reservation_64;
}

}  // namespace

class UserModeGuestCodeDomainCoordinatorState final {
 public:
  UserModeGuestCodeDomainCoordinatorDiagnostic Reserve(
      UserModeMappingHandle mapping) {
    if (mapping.selector >= slots_.size() || mapping.generation == 0) {
      return UserModeGuestCodeDomainCoordinatorDiagnostic::
          kInvalidMappingSnapshot;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[mapping.selector];
    if (slot.registering || slot.lease) {
      return slot.mapping == mapping
                 ? UserModeGuestCodeDomainCoordinatorDiagnostic::
                       kDuplicateMapping
                 : UserModeGuestCodeDomainCoordinatorDiagnostic::
                       kSelectorConflict;
    }
    slot.mapping = mapping;
    slot.registering = true;
    return UserModeGuestCodeDomainCoordinatorDiagnostic::kNone;
  }

  bool Publish(
      UserModeMappingHandle mapping,
      const std::shared_ptr<UserModeGuestCodeDomainLease>& lease) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[mapping.selector];
    if (!slot.registering || slot.mapping != mapping || slot.lease) {
      return false;
    }
    slot.lease = lease;
    slot.registering = false;
    return true;
  }

  void CancelReservation(UserModeMappingHandle mapping) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[mapping.selector];
    if (slot.registering && slot.mapping == mapping && !slot.lease) {
      slot = Slot{};
    }
  }

  void RemoveRetiredLease(UserModeMappingHandle mapping,
                          const UserModeGuestCodeDomainLease* lease) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[mapping.selector];
    if (slot.mapping != mapping) {
      return;
    }
    if (slot.registering && !slot.lease) {
      // Retirement may have run since the listener attached.
      slot = Slot{};
    } else if (slot.lease.get() == lease) {
      slot = Slot{};
    }
  }

  std::shared_ptr<UserModeGuestCodeDomainLease> FindLease(
      UserModeMappingHandle mapping) const {
    if (mapping.selector >= slots_.size() || mapping.generation == 0) {
      return nullptr;
    }

    std::shared_ptr<UserModeGuestCodeDomainLease> lease;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const Slot& slot = slots_[mapping.selector];
      if (slot.mapping != mapping) {
        return nullptr;
      }
      lease = slot.lease;
    }
    return lease && lease->CanBeginExecution() ? lease : nullptr;
  }

  std::shared_ptr<UserModeGuestCodeDomainLease> FindLeaseForDomain(
      cpu::TranslationDomainKey domain) const {
    std::shared_ptr<UserModeGuestCodeDomainLease> lease;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const Slot& slot : slots_) {
        if (slot.lease && slot.lease->ContainsDomain(domain)) {
          lease = slot.lease;
          break;
        }
      }
    }
    return lease && lease->CanBeginExecution() ? lease : nullptr;
  }

  std::vector<std::shared_ptr<UserModeGuestCodeDomainLease>> ActiveLeases()
      const {
    std::vector<std::shared_ptr<UserModeGuestCodeDomainLease>> leases;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const Slot& slot : slots_) {
        if (slot.lease) {
          leases.push_back(slot.lease);
        }
      }
    }
    std::erase_if(
        leases, [](const auto& lease) { return !lease->CanBeginExecution(); });
    return leases;
  }

  size_t active_lease_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const Slot& slot : slots_) {
      count += slot.lease && slot.lease->CanBeginExecution() ? 1 : 0;
    }
    return count;
  }

  void RetireAll() noexcept {
    std::array<std::shared_ptr<UserModeGuestCodeDomainLease>, 2> leases;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (size_t i = 0; i < slots_.size(); ++i) {
        leases[i] = std::move(slots_[i].lease);
        slots_[i] = Slot{};
      }
    }
    for (const auto& lease : leases) {
      if (lease) {
        lease->RetireResources();
      }
    }
  }

 private:
  struct Slot {
    UserModeMappingHandle mapping;
    bool registering = false;
    std::shared_ptr<UserModeGuestCodeDomainLease> lease;
  };

  mutable std::mutex mutex_;
  std::array<Slot, 2> slots_{};
};

class UserModeGuestCodeDomainTransitionPermit final
    : public cpu::TranslationDomainTransitionPermit {
 public:
  UserModeGuestCodeDomainTransitionPermit(
      const UserModeGuestCodeDomainCoordinator* owner,
      std::shared_ptr<UserModeGuestCodeDomainLease> lease,
      cpu::TranslationDomainKey expected_source_domain_key,
      cpu::TranslationDomainKey target_domain_key)
      : owner_(owner),
        lease_(std::move(lease)),
        expected_source_domain_key_(expected_source_domain_key),
        target_domain_key_(target_domain_key) {}

  ~UserModeGuestCodeDomainTransitionPermit() override {
    lease_->ReleaseTransition();
  }

  cpu::TranslationDomainKey expected_source_domain_key()
      const noexcept override {
    return expected_source_domain_key_;
  }
  cpu::TranslationDomainKey target_domain_key() const noexcept override {
    return target_domain_key_;
  }

  const UserModeGuestCodeDomainCoordinator* owner() const { return owner_; }
  const std::shared_ptr<UserModeGuestCodeDomainLease>& lease() const {
    return lease_;
  }

 private:
  const UserModeGuestCodeDomainCoordinator* const owner_;
  const std::shared_ptr<UserModeGuestCodeDomainLease> lease_;
  const cpu::TranslationDomainKey expected_source_domain_key_;
  const cpu::TranslationDomainKey target_domain_key_;
};

UserModeGuestCodeDomainLease::UserModeGuestCodeDomainLease(
    cpu::TranslationDomainRegistry& registry, UserModeMappingHandle mapping,
    std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
    UserModeExecutionDomainKeys domains,
    std::unique_ptr<UserModeExecutionDomainPlanner> planner,
    cpu::Processor* processor, std::array<std::string, 4> mapped_module_names,
    std::weak_ptr<UserModeGuestCodeDomainCoordinatorState> owner_state)
    : registry_(registry),
      mapping_(mapping),
      generation_token_(std::move(generation_token)),
      domains_(domains),
      planner_(std::move(planner)),
      processor_(processor),
      mapped_module_names_(std::move(mapped_module_names)),
      owner_state_(std::move(owner_state)) {}

UserModeGuestCodeDomainLease::~UserModeGuestCodeDomainLease() {
  RetireResources();
}

bool UserModeGuestCodeDomainLease::TryAcquireTransition() noexcept {
  std::lock_guard<std::mutex> lock(transition_mutex_);
  if (!CanBeginExecution()) {
    return false;
  }
  ++active_transition_count_;
  return true;
}

void UserModeGuestCodeDomainLease::ReleaseTransition() noexcept {
  std::lock_guard<std::mutex> lock(transition_mutex_);
  assert_true(active_transition_count_ != 0);
  --active_transition_count_;
  if (active_transition_count_ == 0) {
    transition_condition_.notify_all();
  }
}

bool UserModeGuestCodeDomainLease::TryBeginFlush() noexcept {
  std::unique_lock<std::mutex> lock(transition_mutex_);
  if (!CanBeginExecution()) {
    return false;
  }
  flushing_.store(true, std::memory_order_release);
  if (processor_) {
    processor_->AdvanceCheckedExecutionEpoch();
  }
  transition_condition_.wait(lock,
                             [this] { return active_transition_count_ == 0; });
  if (!active_.load(std::memory_order_acquire) ||
      retiring_.load(std::memory_order_acquire) || !generation_token_ ||
      !generation_token_->is_current()) {
    flushing_.store(false, std::memory_order_release);
    transition_condition_.notify_all();
    return false;
  }
  return true;
}

void UserModeGuestCodeDomainLease::EndFlush() noexcept {
  std::lock_guard<std::mutex> lock(transition_mutex_);
  assert_true(flushing_.load(std::memory_order_acquire));
  flushing_.store(false, std::memory_order_release);
  if (processor_) {
    processor_->AdvanceCheckedExecutionEpoch();
  }
  transition_condition_.notify_all();
}

bool UserModeGuestCodeDomainLease::CanBeginExecution() const noexcept {
  return active_.load(std::memory_order_acquire) &&
         !retiring_.load(std::memory_order_acquire) &&
         !flushing_.load(std::memory_order_acquire) && generation_token_ &&
         generation_token_->is_current();
}

bool UserModeGuestCodeDomainLease::ContainsDomain(
    cpu::TranslationDomainKey domain) const noexcept {
  for (const auto& endian_domains : domains_) {
    for (cpu::TranslationDomainKey candidate : endian_domains) {
      if (candidate == domain) {
        return true;
      }
    }
  }
  return false;
}

void UserModeGuestCodeDomainLease::RetireResources() noexcept {
  // Stop new transitions and wait for selected ones to commit, then close
  // authorization before removing modules and domains.
  std::call_once(retirement_once_, [this]() noexcept {
    {
      std::unique_lock<std::mutex> lock(transition_mutex_);
      retiring_.store(true, std::memory_order_release);
      if (processor_) {
        processor_->AdvanceCheckedExecutionEpoch();
      }
      transition_condition_.wait(lock, [this] {
        return active_transition_count_ == 0 &&
               !flushing_.load(std::memory_order_acquire);
      });
    }
    active_.store(false, std::memory_order_release);
    planner_->UnbindMapping(mapping_);

    bool safe_to_retire = true;
    if (processor_) {
      std::array<cpu::TranslationDomainKey, 4> flattened_domains{};
      size_t flattened_index = 0;
      for (const auto& endian_domains : domains_) {
        for (cpu::TranslationDomainKey domain : endian_domains) {
          flattened_domains[flattened_index++] = domain;
        }
      }
      if (processor_->HasLiveThreadInTranslationDomains(flattened_domains)) {
        safe_to_retire = false;
        XELOGE(
            "Deferring checked mapping retirement while a live thread "
            "retains one of its translation domains");
      } else {
        for (cpu::TranslationDomainKey domain : flattened_domains) {
          const cpu::GuestCodeInvalidationResult invalidation =
              processor_->InvalidateGuestCodeDomain(domain);
          if (!invalidation) {
            safe_to_retire = false;
            XELOGE(
                "Deferring checked domain {:016X} retirement: full "
                "invalidation status {}",
                domain.id(), static_cast<uint32_t>(invalidation.status));
          }
        }
      }
      if (safe_to_retire) {
        RemoveMappedModules(*processor_, domains_, mapped_module_names_);
      }
    }
    if (!processor_ || safe_to_retire) {
      RetireDomainSet(registry_, domains_);
    }
  });
}

void UserModeGuestCodeDomainLease::OnUserModeMappingRetired(
    UserModeMappingHandle mapping) noexcept {
  // The token identifies the mapping.
  (void)mapping;
  RetireResources();
  if (const std::shared_ptr<UserModeGuestCodeDomainCoordinatorState> state =
          owner_state_.lock()) {
    state->RemoveRetiredLease(mapping_, this);
  }
}

UserModeGuestCodeDomainCoordinator::UserModeGuestCodeDomainCoordinator(
    Memory& memory, cpu::TranslationDomainRegistry& registry,
    UserModeGuestCodeDomainCoordinatorMode mode)
    : memory_(memory),
      registry_(registry),
      mode_(mode),
      state_(std::make_shared<UserModeGuestCodeDomainCoordinatorState>()),
      processor_(registry.processor()),
      processor_dispatcher_installed_(
          mode == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled &&
          processor_ && processor_->backend() &&
          processor_->backend()->SupportsCheckedGuestMemoryAccess() &&
          processor_->InstallCheckedGuestMemoryDispatcher(this)) {}

UserModeGuestCodeDomainCoordinator::~UserModeGuestCodeDomainCoordinator() {
  checked_guest_memory_fault_handler_.store(nullptr, std::memory_order_release);
  state_->RetireAll();
  if (processor_dispatcher_installed_) {
    processor_->UninstallCheckedGuestMemoryDispatcher(this);
  }
}

bool UserModeGuestCodeDomainCoordinator::InstallCheckedGuestMemoryFaultHandler(
    cpu::CheckedGuestMemoryFaultHandler* handler) {
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_dispatcher_installed_ || !handler || handler == this) {
    return false;
  }
  cpu::CheckedGuestMemoryFaultHandler* expected = nullptr;
  return checked_guest_memory_fault_handler_.compare_exchange_strong(
      expected, handler, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool UserModeGuestCodeDomainCoordinator::
    UninstallCheckedGuestMemoryFaultHandler(
        cpu::CheckedGuestMemoryFaultHandler* handler) {
  if (!handler) {
    return false;
  }
  cpu::CheckedGuestMemoryFaultHandler* expected = handler;
  return checked_guest_memory_fault_handler_.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

UserModeGuestCodeDomainCoordinatorResult
UserModeGuestCodeDomainCoordinator::RegisterMapping(
    const UserModeMappingSnapshot& mapping_snapshot) {
  UserModeGuestCodeDomainCoordinatorResult result;
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kDisabled) {
    result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::kDisabled;
    return result;
  }
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled &&
      (!processor_ || !processor_->backend() ||
       !processor_->backend()->SupportsCheckedGuestMemoryAccess())) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kProcessorUnavailable;
    return result;
  }
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled &&
      (!processor_dispatcher_installed_ ||
       !checked_guest_memory_fault_handler())) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kDispatcherUnavailable;
    return result;
  }
  if (!IsValidMappingSnapshot(mapping_snapshot)) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kInvalidMappingSnapshot;
    return result;
  }
  if (!mapping_snapshot.generation_token->is_current()) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kMappingNotCurrent;
    return result;
  }

  result.diagnostic = state_->Reserve(mapping_snapshot.handle);
  if (result.diagnostic !=
      UserModeGuestCodeDomainCoordinatorDiagnostic::kNone) {
    return result;
  }

  const UserModeGuestCodeDomainRegistrationResult registration =
      RegisterUserModeGuestCodeDomainsInternal(
          memory_, registry_, mapping_snapshot,
          mode_ == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled
              ? UserModeGuestDataStoreMode::kLiveCheckedExecution
              : UserModeGuestDataStoreMode::kDisabled);
  if (!registration) {
    state_->CancelReservation(mapping_snapshot.handle);
    result.domain_registration_diagnostic = registration.diagnostic;
    switch (registration.diagnostic) {
      case UserModeGuestCodeDomainRegistrationDiagnostic::
          kInvalidMappingSnapshot:
        result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::
            kInvalidMappingSnapshot;
        break;
      case UserModeGuestCodeDomainRegistrationDiagnostic::kMappingNotCurrent:
        result.diagnostic =
            UserModeGuestCodeDomainCoordinatorDiagnostic::kMappingNotCurrent;
        break;
      case UserModeGuestCodeDomainRegistrationDiagnostic::kRegistrationFailed:
        result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::
            kDomainRegistrationFailed;
        break;
      case UserModeGuestCodeDomainRegistrationDiagnostic::kNone:
        result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::
            kDomainRegistrationFailed;
        break;
    }
    return result;
  }

  const UserModeExecutionDomainKeys domains = *registration.domains;
  std::array<std::string, 4> mapped_module_names{};
  bool mapped_modules_installed = false;
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled) {
    mapped_modules_installed =
        InstallMappedModules(*processor_, domains, &mapped_module_names);
    if (!mapped_modules_installed) {
      state_->CancelReservation(mapping_snapshot.handle);
      RetireDomainSet(registry_, domains);
      result.diagnostic =
          UserModeGuestCodeDomainCoordinatorDiagnostic::kProcessorUnavailable;
      return result;
    }
  }
  std::unique_ptr<UserModeExecutionDomainPlanner> planner;
  bool planner_bound = false;
  std::shared_ptr<UserModeGuestCodeDomainLease> lease;
  try {
    planner = std::make_unique<UserModeExecutionDomainPlanner>(
        registry_, UserModeExecutionDomainPlannerMode::kSyntheticPlanOnly);
    const UserModeExecutionDomainDiagnostic planner_diagnostic =
        planner->BindMapping(mapping_snapshot.handle, domains);
    result.planner_diagnostic = planner_diagnostic;
    if (planner_diagnostic != UserModeExecutionDomainDiagnostic::kNone) {
      state_->CancelReservation(mapping_snapshot.handle);
      if (mapped_modules_installed) {
        RemoveMappedModules(*processor_, domains, mapped_module_names);
      }
      RetireDomainSet(registry_, domains);
      result.diagnostic =
          UserModeGuestCodeDomainCoordinatorDiagnostic::kPlannerBindingFailed;
      return result;
    }
    planner_bound = true;
    lease = std::shared_ptr<UserModeGuestCodeDomainLease>(
        new UserModeGuestCodeDomainLease(
            registry_, mapping_snapshot.handle,
            mapping_snapshot.generation_token, domains, std::move(planner),
            mapped_modules_installed ? processor_ : nullptr,
            std::move(mapped_module_names), state_));
  } catch (...) {
    if (planner_bound && planner) {
      planner->UnbindMapping(mapping_snapshot.handle);
    }
    if (mapped_modules_installed) {
      RemoveMappedModules(*processor_, domains, mapped_module_names);
    }
    RetireDomainSet(registry_, domains);
    state_->CancelReservation(mapping_snapshot.handle);
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kLeaseCreationFailed;
    return result;
  }

  const UserModeMappingRetirementListenerAttachmentDiagnostic
      attachment_diagnostic =
          mapping_snapshot.generation_token->AttachRetirementListener(lease);
  if (attachment_diagnostic !=
      UserModeMappingRetirementListenerAttachmentDiagnostic::kNone) {
    result.listener_attachment_diagnostic = attachment_diagnostic;
    state_->CancelReservation(mapping_snapshot.handle);
    lease->RetireResources();
    result.diagnostic =
        attachment_diagnostic ==
                UserModeMappingRetirementListenerAttachmentDiagnostic::
                    kMappingNotCurrent
            ? UserModeGuestCodeDomainCoordinatorDiagnostic::kMappingNotCurrent
            : UserModeGuestCodeDomainCoordinatorDiagnostic::
                  kRetirementListenerAttachmentFailed;
    return result;
  }

  // Publication can't fail; it refuses a lease retirement already took.
  if (!state_->Publish(mapping_snapshot.handle, lease)) {
    lease->RetireResources();
    result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::
        kMappingRetiredDuringRegistration;
    return result;
  }

  // A Delete may have run since attachment; finish its cleanup if so.
  if (!mapping_snapshot.generation_token->is_current() || !lease->is_active()) {
    lease->RetireResources();
    state_->RemoveRetiredLease(mapping_snapshot.handle, lease.get());
  }

  result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::kNone;
  result.execution_permitted =
      mode_ == UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled &&
      lease->CanBeginExecution() && checked_guest_memory_fault_handler();
  result.lease = std::move(lease);
  return result;
}

std::shared_ptr<const UserModeGuestCodeDomainLease>
UserModeGuestCodeDomainCoordinator::FindLease(
    UserModeMappingHandle mapping) const {
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kDisabled) {
    return nullptr;
  }
  return state_->FindLease(mapping);
}

UserModeGuestCodeDomainTransitionResult
UserModeGuestCodeDomainCoordinator::AcquireTransition(
    const UserModeExecutionDomainPlan& plan) const {
  if (!plan.IsStructurallyValid()) {
    UserModeGuestCodeDomainTransitionResult result;
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kTransitionUnavailable;
    return result;
  }
  return AcquireTransition(plan.mapping, plan.expected_source_domain_key,
                           plan.target_domain_key);
}

UserModeGuestCodeDomainTransitionResult
UserModeGuestCodeDomainCoordinator::AcquireTransition(
    UserModeMappingHandle mapping,
    cpu::TranslationDomainKey expected_source_domain_key,
    cpu::TranslationDomainKey target_domain_key) const {
  UserModeGuestCodeDomainTransitionResult result;
  result.mapping = mapping;
  result.expected_source_domain_key = expected_source_domain_key;
  result.target_domain_key = target_domain_key;

  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kDisabled) {
    result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::kDisabled;
    return result;
  }
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_ || !processor_dispatcher_installed_ ||
      !checked_guest_memory_fault_handler()) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kDispatcherUnavailable;
    return result;
  }

  std::shared_ptr<UserModeGuestCodeDomainLease> lease =
      state_->FindLease(mapping);
  if (!lease ||
      (expected_source_domain_key !=
           cpu::TranslationDomainKey::DefaultDirect() &&
       !lease->ContainsDomain(expected_source_domain_key)) ||
      (target_domain_key != cpu::TranslationDomainKey::DefaultDirect() &&
       !lease->ContainsDomain(target_domain_key)) ||
      (expected_source_domain_key ==
           cpu::TranslationDomainKey::DefaultDirect() &&
       target_domain_key == cpu::TranslationDomainKey::DefaultDirect())) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kTransitionUnavailable;
    return result;
  }
  if (!lease->TryAcquireTransition()) {
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kTransitionUnavailable;
    return result;
  }

  try {
    result.permit =
        std::make_shared<const UserModeGuestCodeDomainTransitionPermit>(
            this, lease, expected_source_domain_key, target_domain_key);
  } catch (...) {
    lease->ReleaseTransition();
    result.diagnostic =
        UserModeGuestCodeDomainCoordinatorDiagnostic::kTransitionUnavailable;
    return result;
  }
  result.execution_permitted = true;
  result.diagnostic = UserModeGuestCodeDomainCoordinatorDiagnostic::kNone;
  return result;
}

UserModeGuestCodeDomainFlushResult
UserModeGuestCodeDomainCoordinator::InvalidateMappingDomains(
    UserModeMappingHandle mapping) {
  UserModeGuestCodeDomainFlushResult result;
  if (mode_ == UserModeGuestCodeDomainCoordinatorMode::kDisabled) {
    result.diagnostic = UserModeGuestCodeDomainFlushDiagnostic::kDisabled;
    return result;
  }
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_ || !processor_dispatcher_installed_) {
    result.diagnostic =
        UserModeGuestCodeDomainFlushDiagnostic::kProcessorUnavailable;
    return result;
  }

  std::shared_ptr<UserModeGuestCodeDomainLease> lease =
      state_->FindLease(mapping);
  if (!lease || !lease->TryBeginFlush()) {
    result.diagnostic =
        UserModeGuestCodeDomainFlushDiagnostic::kMappingUnavailable;
    return result;
  }

  std::array<cpu::TranslationDomainKey, 4> domains{};
  size_t domain_index = 0;
  for (const auto& endian_domains : lease->domains()) {
    for (cpu::TranslationDomainKey domain : endian_domains) {
      domains[domain_index++] = domain;
    }
  }

  if (processor_->HasLiveThreadInTranslationDomains(domains)) {
    lease->EndFlush();
    result.diagnostic =
        UserModeGuestCodeDomainFlushDiagnostic::kLiveTargetThreads;
    return result;
  }

  for (size_t i = 0; i < domains.size(); ++i) {
    result.domain_results[i] =
        processor_->InvalidateGuestCodeDomain(domains[i]);
    if (!result.domain_results[i]) {
      lease->EndFlush();
      result.diagnostic =
          result.domain_results[i].status ==
                  cpu::GuestCodeInvalidationStatus::kLiveGuestThreads
              ? UserModeGuestCodeDomainFlushDiagnostic::kLiveTargetThreads
              : UserModeGuestCodeDomainFlushDiagnostic::kInvalidationFailed;
      return result;
    }
    ++result.invalidated_domain_count;
  }

  lease->EndFlush();
  result.diagnostic = UserModeGuestCodeDomainFlushDiagnostic::kNone;
  return result;
}

bool UserModeGuestCodeDomainCoordinator::IsDomainExecutionAuthorized(
    cpu::TranslationDomainKey translation_domain_key,
    const cpu::TranslationDomainDescriptor& descriptor) const noexcept {
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_dispatcher_installed_ ||
      !checked_guest_memory_fault_handler() ||
      descriptor.translation_domain_key() != translation_domain_key ||
      registry_.Lookup(translation_domain_key) != &descriptor ||
      !HasLiveCheckedMemoryCapabilities(descriptor)) {
    return false;
  }
  const std::shared_ptr<UserModeGuestCodeDomainLease> lease =
      state_->FindLeaseForDomain(translation_domain_key);
  return lease && lease->CanBeginExecution();
}

bool UserModeGuestCodeDomainCoordinator::IsDomainTransitionAuthorized(
    cpu::TranslationDomainKey current_domain_key,
    cpu::TranslationDomainKey target_domain_key,
    const cpu::TranslationDomainDescriptor* target_descriptor,
    const cpu::TranslationDomainTransitionPermit& permit) const noexcept {
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_dispatcher_installed_ ||
      !checked_guest_memory_fault_handler()) {
    return false;
  }
  const auto* user_mode_permit =
      dynamic_cast<const UserModeGuestCodeDomainTransitionPermit*>(&permit);
  if (!user_mode_permit || user_mode_permit->owner() != this ||
      permit.expected_source_domain_key() != current_domain_key ||
      permit.target_domain_key() != target_domain_key) {
    return false;
  }

  const std::shared_ptr<UserModeGuestCodeDomainLease>& lease =
      user_mode_permit->lease();
  if (!lease || !lease->is_active() ||
      (current_domain_key != cpu::TranslationDomainKey::DefaultDirect() &&
       (!lease->ContainsDomain(current_domain_key) ||
        !registry_.Lookup(current_domain_key)))) {
    return false;
  }
  if (target_domain_key == cpu::TranslationDomainKey::DefaultDirect()) {
    return target_descriptor == nullptr;
  }
  return lease->ContainsDomain(target_domain_key) && target_descriptor &&
         target_descriptor->translation_domain_key() == target_domain_key &&
         registry_.Lookup(target_domain_key) == target_descriptor;
}

[[noreturn]] void UserModeGuestCodeDomainCoordinator::HandleGuestMemoryFault(
    cpu::ppc::PPCContext* context,
    cpu::TranslationDomainKey translation_domain_key,
    const cpu::GuestMemoryAccessRequest& request,
    const cpu::GuestMemoryAccessResult& result) {
  cpu::CheckedGuestMemoryFaultHandler* handler =
      checked_guest_memory_fault_handler();
  if (handler && handler != this) {
    handler->HandleGuestMemoryFault(context, translation_domain_key, request,
                                    result);
  }
  XELOGE("Checked guest memory fault had no live runtime handler");
  std::abort();
}

bool UserModeGuestCodeDomainCoordinator::DispatchGuestInstructionStorageFault(
    cpu::ppc::PPCContext* context,
    cpu::TranslationDomainKey translation_domain_key,
    const cpu::ppc::GuestInstructionFetchFault& fault) {
  cpu::CheckedGuestMemoryFaultHandler* handler =
      checked_guest_memory_fault_handler();
  return handler && handler != this &&
         handler->DispatchGuestInstructionStorageFault(
             context, translation_domain_key, fault);
}

namespace {

cpu::GuestCodeInvalidationResult InvalidateLeaseDomainsLive(
    cpu::Processor* processor,
    std::span<const std::shared_ptr<UserModeGuestCodeDomainLease>> leases) {
  // Data accesses stop using the flushed translations too.
  UserModeGuestDataAccessor::InvalidateTranslationCaches();
  std::vector<cpu::TranslationDomainKey> domains;
  for (const auto& lease : leases) {
    for (const auto& endian_domains : lease->domains()) {
      domains.insert(domains.end(), endian_domains.begin(),
                     endian_domains.end());
    }
  }
  cpu::GuestCodeInvalidationResult result;
  if (domains.empty()) {
    result.status = cpu::GuestCodeInvalidationStatus::kSuccess;
    return result;
  }
  // A TB flush changes address translations only: retire just the
  // translations whose code no longer fetches from the same physical bytes.
  return processor->InvalidateRemappedCheckedGuestCodeDomains(domains);
}

}  // namespace

cpu::GuestCodeInvalidationResult
UserModeGuestCodeDomainCoordinator::InvalidateMappingDomainsLive(
    UserModeMappingHandle mapping) {
  // A TB flush drops cached data translations whether or not code can be
  // invalidated.
  UserModeGuestDataAccessor::InvalidateTranslationCaches();
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_) {
    cpu::GuestCodeInvalidationResult result;
    result.status =
        cpu::GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
    return result;
  }
  std::shared_ptr<UserModeGuestCodeDomainLease> lease =
      state_->FindLease(mapping);
  if (!lease) {
    cpu::GuestCodeInvalidationResult result;
    result.status =
        cpu::GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
    return result;
  }
  return InvalidateLeaseDomainsLive(processor_, std::span(&lease, 1));
}

cpu::GuestCodeInvalidationResult
UserModeGuestCodeDomainCoordinator::InvalidateAllMappingDomainsLive() {
  // A TB flush drops cached data translations whether or not code can be
  // invalidated.
  UserModeGuestDataAccessor::InvalidateTranslationCaches();
  if (mode_ != UserModeGuestCodeDomainCoordinatorMode::kExecutionEnabled ||
      !processor_) {
    cpu::GuestCodeInvalidationResult result;
    result.status =
        cpu::GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
    return result;
  }
  return InvalidateLeaseDomainsLive(processor_, state_->ActiveLeases());
}

size_t UserModeGuestCodeDomainCoordinator::active_lease_count() const {
  return mode_ == UserModeGuestCodeDomainCoordinatorMode::kDisabled
             ? 0
             : state_->active_lease_count();
}

}  // namespace xe::kernel

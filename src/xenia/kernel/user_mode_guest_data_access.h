/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_GUEST_DATA_ACCESS_H_
#define XENIA_KERNEL_USER_MODE_GUEST_DATA_ACCESS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "xenia/cpu/guest_memory_access.h"
#include "xenia/kernel/user_mode_address_space.h"
#include "xenia/kernel/user_mode_manager.h"
#include "xenia/kernel/user_mode_physical_memory.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

enum class UserModeGuestDataOperand : uint8_t {
  kScalar8,
  kScalar16,
  kScalar32,
  kScalar64,
  kVector128,
};

struct UserModeGuestDataTransferFragment {
  uint32_t storage_effective_address = 0;
  uint32_t byte_count = 0;
  uint32_t canonical_byte_offset = 0;
};

enum class UserModeGuestDataTransferPlanDiagnostic {
  kNone,
  kInvalidOperand,
  kUnalignedEffectiveAddress,
};

struct UserModeGuestDataTransferPlan {
  UserModeGuestDataTransferPlanDiagnostic diagnostic =
      UserModeGuestDataTransferPlanDiagnostic::kNone;
  UserModeGuestDataOperand operand = UserModeGuestDataOperand::kScalar8;
  uint32_t effective_address = 0;
  uint32_t canonical_byte_count = 0;
  size_t fragment_count = 0;
  std::array<UserModeGuestDataTransferFragment, 2> fragments{};
  bool little_endian = false;

  explicit operator bool() const {
    return diagnostic == UserModeGuestDataTransferPlanDiagnostic::kNone &&
           canonical_byte_count != 0 && fragment_count != 0;
  }
};

// Plans the transfer of a naturally aligned operand. Canonical byte offsets
// are in architectural (most significant first) order.
UserModeGuestDataTransferPlan PlanUserModeGuestDataTransfer(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    bool little_endian);

enum class UserModeGuestDataAccessDiagnostic {
  kNone,
  kInvalidConfiguration,
  kInvalidOperand,
  kUnalignedEffectiveAddress,
  kStoreDisabled,
  kInvalidCanonicalInput,
  kMappingNotCurrent,
  kAddressTranslationFailed,
  kGuestAccessFault,
  kPhysicalBackingFailed,
  kInvalidationNotificationFailed,
  kTranslationChangedDuringCommit,
  kUnsupportedWriteTopology,
  kPteCompareExchangeFailed,
};

enum class UserModeGuestDataPhysicalAccessStage {
  kNone,
  kRootEntry,
  kPageTableEntry,
  kOperandFragment,
  kPteChangedBitWriteback,
};

struct UserModeGuestDataReadResult {
  UserModeGuestDataAccessDiagnostic diagnostic =
      UserModeGuestDataAccessDiagnostic::kNone;
  UserModeGuestDataTransferPlan plan;
  std::array<std::optional<UserModeAddressTranslationResult>, 2> translations;
  std::optional<size_t> failure_fragment_index;
  UserModeGuestDataPhysicalAccessStage physical_failure_stage =
      UserModeGuestDataPhysicalAccessStage::kNone;
  std::optional<UserModePhysicalReadResult> physical_read_failure;
  std::optional<std::array<uint8_t, 16>> canonical_bytes;

  explicit operator bool() const {
    return diagnostic == UserModeGuestDataAccessDiagnostic::kNone &&
           static_cast<bool>(plan) && canonical_bytes.has_value();
  }
};

struct UserModeGuestDataPteWritebackRequirement {
  uint32_t pte_physical_address = 0;
  uint32_t observed_raw_pte = 0;
  uint32_t writeback_raw_pte = 0;
  bool changed_bit_writeback_required = false;
};

// The backing a load-reserve saw, as translation fields rather than a hash of
// the bytes, so a remap fails the matching store-conditional.
struct UserModeGuestDataReservationIdentity {
  uint32_t storage_effective_address = 0;
  uint32_t physical_address = 0;
  uint32_t pte_physical_address = 0;
  // Mapping/protection identity with the independently mutable C/R bits
  // removed, so a precise retry after this store's C update stays valid.
  uint32_t stable_raw_pte = 0;

  bool operator==(
      const UserModeGuestDataReservationIdentity& other) const noexcept {
    return storage_effective_address == other.storage_effective_address &&
           physical_address == other.physical_address &&
           pte_physical_address == other.pte_physical_address &&
           stable_raw_pte == other.stable_raw_pte;
  }
};

struct UserModeGuestDataWriteProbeResult {
  UserModeGuestDataAccessDiagnostic diagnostic =
      UserModeGuestDataAccessDiagnostic::kNone;
  UserModeGuestDataTransferPlan plan;
  std::array<std::optional<UserModeAddressTranslationResult>, 2> translations;
  std::optional<size_t> failure_fragment_index;
  UserModeGuestDataPhysicalAccessStage physical_failure_stage =
      UserModeGuestDataPhysicalAccessStage::kNone;
  std::optional<UserModePhysicalReadResult> physical_read_failure;
  std::optional<UserModePhysicalWriteProbeResult> physical_write_probe_failure;
  // The C-clear PTEs the store must update; set only when the whole probe
  // succeeds.
  std::array<UserModeGuestDataPteWritebackRequirement, 2>
      pte_writeback_requirements{};
  size_t pte_writeback_requirement_count = 0;

  static constexpr bool execution_permitted = false;
  static constexpr bool write_performed = false;

  explicit operator bool() const {
    return diagnostic == UserModeGuestDataAccessDiagnostic::kNone &&
           static_cast<bool>(plan);
  }
};

// Synthetic commit requires quiesced guest code.
enum class UserModeGuestDataStoreMode : uint8_t {
  kDisabled,
  kSyntheticCommitOnly,
  // Live execution: physical write observers run before the store, and mapping
  // retirement waits for it. V128 stores aren't atomic.
  kLiveCheckedExecution,
};

enum class UserModeGuestDataPartialVectorStore : uint8_t {
  kLeft,
  kRight,
};

enum class UserModeGuestDataStoreStage : uint8_t {
  kNone,
  kInitialPreflight,
  kInvalidationNotification,
  kOperationLease,
  kCommitPreflight,
  kPteChangedBitCompareExchange,
  kOperandCopy,
};

struct UserModeGuestDataStoreResult {
  UserModeGuestDataAccessDiagnostic diagnostic =
      UserModeGuestDataAccessDiagnostic::kNone;
  UserModeGuestDataStoreStage stage = UserModeGuestDataStoreStage::kNone;
  UserModeGuestDataTransferPlan plan;
  std::array<std::optional<UserModeAddressTranslationResult>, 2>
      initial_translations;
  std::array<std::optional<UserModeAddressTranslationResult>, 2>
      commit_translations;
  std::optional<size_t> failure_fragment_index;
  UserModeGuestDataPhysicalAccessStage physical_failure_stage =
      UserModeGuestDataPhysicalAccessStage::kNone;
  std::optional<UserModePhysicalReadResult> physical_read_failure;
  std::optional<UserModePhysicalWriteProbeResult> physical_write_probe_failure;
  std::optional<uint32_t> pte_compare_exchange_observed_raw;
  // Number of exact physical ranges dispatched, not the number of registered
  // callbacks that observed each range.
  size_t invalidation_notification_count = 0;
  bool changed_bit_writeback_performed = false;
  bool write_performed = false;
  std::optional<bool> atomic_predicate;

  // Never authorizes execution.
  static constexpr bool execution_permitted = false;
  static constexpr bool synthetic_external_byte_writers_must_be_quiesced = true;
  static constexpr bool external_byte_writers_must_be_quiesced =
      synthetic_external_byte_writers_must_be_quiesced;

  explicit operator bool() const {
    return diagnostic == UserModeGuestDataAccessDiagnostic::kNone &&
           static_cast<bool>(plan) &&
           (write_performed || atomic_predicate.has_value());
  }
};

// Checked data access over one mapping generation. Reads return complete
// canonical values; write probes check permission and backing without
// changing anything. Stores commit synthetically (guest code quiesced) or
// live, where the physical write observers run before the store. V128 stores
// aren't atomic.
class UserModeGuestDataAccessor final {
 public:
  UserModeGuestDataAccessor(
      Memory* memory,
      std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
      UserModeProblemStateKey problem_state_key, bool little_endian,
      UserModeGuestDataStoreMode store_mode =
          UserModeGuestDataStoreMode::kDisabled);
  ~UserModeGuestDataAccessor();

  UserModeGuestDataReadResult ReadAligned(UserModeGuestDataOperand operand,
                                          uint32_t effective_address) const;

  UserModeGuestDataWriteProbeResult ProbeAlignedWrite(
      UserModeGuestDataOperand operand, uint32_t effective_address) const;

  // Stores the operand and, if needed, sets the PTE's C bit atomically. On
  // failure no byte is written, though write observers may have been
  // notified.
  UserModeGuestDataStoreResult CommitSyntheticAlignedStore(
      UserModeGuestDataOperand operand, uint32_t effective_address,
      std::span<const uint8_t> canonical_input) const;

  // Stores only the bytes stvl/stvr select. A right store at offset 0 is a
  // no-op the caller handles.
  UserModeGuestDataStoreResult CommitSyntheticPartialVectorStore(
      UserModeGuestDataPartialVectorStore operation, uint32_t effective_address,
      std::span<const uint8_t> architectural_vector_input) const;

  // The zero fill of dcbz; the range is aligned to its size, so it lies
  // within one page.
  UserModeGuestDataStoreResult CommitSyntheticAlignedMemset(
      uint32_t effective_address, uint8_t value, uint32_t byte_count) const;

  UserModeGuestDataStoreResult CompareExchangeSyntheticAligned(
      UserModeGuestDataOperand operand, uint32_t effective_address,
      std::span<const uint8_t> compare_bytes,
      std::span<const uint8_t> replacement_bytes,
      const UserModeGuestDataReservationIdentity*
          required_reservation_identity = nullptr) const;

  // ReadAligned and CommitSyntheticAlignedStore through this thread's cached
  // translations, walking the page table only on a miss. A cached read
  // reports no translations; stores still notify the write observers and hold
  // the operation lease. The translations live in checked_tlb, or in a
  // thread-local table when it is null.
  UserModeGuestDataReadResult ReadAlignedCached(
      UserModeGuestDataOperand operand, uint32_t effective_address,
      cpu::CheckedTlb* checked_tlb = nullptr) const;
  UserModeGuestDataStoreResult StoreAlignedCached(
      UserModeGuestDataOperand operand, uint32_t effective_address,
      std::span<const uint8_t> canonical_input,
      cpu::CheckedTlb* checked_tlb = nullptr) const;
  // CommitSyntheticAlignedMemset through a cached write translation.
  UserModeGuestDataStoreResult MemsetAlignedCached(
      uint32_t effective_address, uint8_t value, uint32_t byte_count,
      cpu::CheckedTlb* checked_tlb = nullptr) const;
  // Whether this thread holds a write translation for the byte's page.
  bool HasCachedWriteTranslation(uint32_t effective_address,
                                 cpu::CheckedTlb* checked_tlb = nullptr) const;
  // Big-endian storage only: copies the bytes at any alignment, across at
  // most one page boundary, when this thread caches every page they touch.
  // False (and nothing copied) otherwise; the caller takes the checked path.
  bool TryReadBytesCached(uint32_t effective_address,
                          std::span<uint8_t> destination,
                          cpu::CheckedTlb* checked_tlb = nullptr) const;
  bool TryWriteBytesCached(uint32_t effective_address,
                           std::span<const uint8_t> source,
                           cpu::CheckedTlb* checked_tlb = nullptr) const;

  // Drops every thread's cached translations (a TB flush).
  static void InvalidateTranslationCaches();

  // The id this accessor's cached translations carry. Never reused.
  uint32_t tlb_accessor_id() const { return tlb_accessor_id_; }

  UserModeMappingHandle mapping_handle() const {
    return generation_token_ ? generation_token_->handle()
                             : UserModeMappingHandle{};
  }
  UserModeProblemStateKey problem_state_key() const {
    return problem_state_key_;
  }
  bool little_endian() const { return little_endian_; }
  UserModeGuestDataStoreMode store_mode() const { return store_mode_; }
  bool is_mapping_current() const { return IsMappingCurrent(); }

 private:
  UserModeGuestDataStoreResult CommitSyntheticPlannedStore(
      UserModeGuestDataTransferPlan plan,
      std::span<const uint8_t> canonical_input) const;
  bool HasValidConfiguration() const;
  bool IsMappingCurrent() const;

  Memory* const memory_;
  const std::shared_ptr<const UserModeMappingGenerationToken> generation_token_;
  const UserModeProblemStateKey problem_state_key_;
  const bool little_endian_;
  const UserModeGuestDataStoreMode store_mode_;
  const uint32_t tlb_accessor_id_;
};

// Creates the GuestMemoryAccessor of a checked domain. It supports stores only
// in the synthetic or live store mode.
std::shared_ptr<const cpu::GuestMemoryAccessor>
CreateUserModeGuestMemoryAccessor(
    Memory* memory,
    std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
    UserModeProblemStateKey problem_state_key, bool little_endian,
    UserModeGuestDataStoreMode store_mode =
        UserModeGuestDataStoreMode::kDisabled);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_GUEST_DATA_ACCESS_H_

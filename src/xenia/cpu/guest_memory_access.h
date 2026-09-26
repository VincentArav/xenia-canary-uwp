/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_MEMORY_ACCESS_H_
#define XENIA_CPU_GUEST_MEMORY_ACCESS_H_

#include <array>
#include <cstdint>
#include <span>

#include "xenia/cpu/translation_domain.h"

namespace xe::cpu {
struct CheckedTlb;
}  // namespace xe::cpu

namespace xe::cpu {

class TranslationDomainDescriptor;

// Operations that may touch guest data memory. MMIO has none and fails checked
// translation, cache-control hints are no-ops, and a memory barrier needs no
// accessor request.
enum class GuestMemoryAccessOperation : uint8_t {
  kLoad,
  kStore,
  kLoadVectorLeft,
  kLoadVectorRight,
  kStoreVectorLeft,
  kStoreVectorRight,
  kMemset,
  kAtomicCompareExchange,
  kLoadReserve,
  kStoreConditional,
};

enum class GuestMemoryOperandType : uint8_t {
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kFloat32,
  kFloat64,
  kVector128,
};

constexpr uint32_t GuestMemoryOperandTypeBit(
    GuestMemoryOperandType operand_type) noexcept {
  return uint32_t{1} << static_cast<uint32_t>(operand_type);
}

constexpr uint32_t GuestMemoryOperandByteCount(
    GuestMemoryOperandType operand_type) noexcept {
  switch (operand_type) {
    case GuestMemoryOperandType::kInt8:
      return 1;
    case GuestMemoryOperandType::kInt16:
      return 2;
    case GuestMemoryOperandType::kInt32:
    case GuestMemoryOperandType::kFloat32:
      return 4;
    case GuestMemoryOperandType::kInt64:
    case GuestMemoryOperandType::kFloat64:
      return 8;
    case GuestMemoryOperandType::kVector128:
      return 16;
  }
  return 0;
}

// What an accessor supports; translating an unsupported operation fails. The
// scalar and vector masks cover plain loads and stores.
struct GuestMemoryAccessCapabilities {
  // Whether the accessor serves live execution rather than the quiesced
  // synthetic mode.
  bool live_checked_execution = false;
  uint32_t load_operand_types = 0;
  uint32_t store_operand_types = 0;
  bool vector_left_right_load = false;
  bool vector_left_right_store = false;
  bool memset = false;
  bool atomic_compare_exchange_32 = false;
  bool atomic_compare_exchange_64 = false;
  bool reservation_32 = false;
  bool reservation_64 = false;
};

// The backing a load-reserve observed, opaque to all but the accessor. The
// matching store-conditional passes it back; the accessor rejects a stale one
// without changing guest state.
struct GuestMemoryReservationIdentity {
  std::array<uint32_t, 4> words{};

  bool operator==(const GuestMemoryReservationIdentity& other) const noexcept {
    return words == other.words;
  }
};

struct GuestMemoryAccessRequest {
  GuestMemoryAccessOperation operation = GuestMemoryAccessOperation::kLoad;
  GuestMemoryOperandType operand_type = GuestMemoryOperandType::kInt8;
  uint32_t instruction_address = 0;
  uint32_t effective_address = 0;

  // Must equal GuestMemoryOperandByteCount for all operations except memset.
  // For memset it is the exact number of bytes to fill.
  uint32_t byte_count = 1;
  uint8_t fill_value = 0;

  // Store and store-conditional consume input_bytes; compare-exchange consumes
  // compare_bytes and input_bytes. Scalar and vector bytes are in ascending
  // guest address order (the HIR value before LOAD_STORE_BYTE_SWAP); partial
  // vectors are in architectural register order.
  std::array<uint8_t, 16> input_bytes{};
  std::array<uint8_t, 16> compare_bytes{};
  GuestMemoryReservationIdentity reservation_identity;
  bool reservation_identity_valid = false;

  // The checked TLB of the context the access runs on, if any. The accessor
  // caches the translations it walks there.
  CheckedTlb* checked_tlb = nullptr;
};

enum class GuestMemoryAccessDiagnostic : uint8_t {
  kNone,
  kInvalidRequest,
  kUnsupportedRequest,
  kDomainUnavailable,
  kMappingRetired,
  kGuestFault,
  kBackingUnavailable,
  kCommitRace,
  kStoreDisabled,
  kInternalFailure,
  kInvalidAccessorResult,
};

enum class GuestMemoryAccessFaultReason : uint8_t {
  kNone,
  kAlignment,
  kAddressTranslation,
  kProtection,
  kPhysicalBacking,
  kMappingRetired,
  kCommitRace,
};

enum class GuestMemoryAccessFaultStage : uint8_t {
  kNone,
  kRequest,
  kRootEntry,
  kPageTableEntry,
  kOperand,
  kPteChangedBitWriteback,
  kInvalidation,
  kCommit,
};

// A fault. backend_diagnostic carries the accessor's own diagnostic code.
struct GuestMemoryAccessFault {
  GuestMemoryAccessFaultReason reason = GuestMemoryAccessFaultReason::kNone;
  GuestMemoryAccessFaultStage stage = GuestMemoryAccessFaultStage::kNone;
  uint32_t instruction_address = 0;
  uint32_t effective_address = 0;
  uint32_t byte_count = 0;
  uint32_t backend_diagnostic = 0;
};

struct GuestMemoryAccessResult {
  GuestMemoryAccessDiagnostic diagnostic = GuestMemoryAccessDiagnostic::kNone;
  GuestMemoryAccessFault fault;
  std::array<uint8_t, 16> output_bytes{};
  // Uses the same backend-neutral architectural byte order as request input.
  uint32_t output_byte_count = 0;

  // Atomic compare-exchange and store-conditional publish their success bit
  // here. Other operations leave it false.
  bool predicate = false;
  // Load-reserve publishes the exact accessor-opaque backing identity here.
  GuestMemoryReservationIdentity reservation_identity;
  bool reservation_identity_valid = false;

  explicit operator bool() const noexcept {
    return diagnostic == GuestMemoryAccessDiagnostic::kNone;
  }
};

class GuestMemoryAccessor {
 public:
  virtual ~GuestMemoryAccessor() = default;

  virtual MemoryAccessMode memory_access_mode() const noexcept = 0;
  virtual GuestMemoryAccessCapabilities capabilities() const noexcept = 0;
  virtual GuestMemoryAccessResult Access(
      const GuestMemoryAccessRequest& request) const noexcept = 0;

  // A plain load or store of byte_count canonical bytes that can complete
  // without the full request (a translation cached in checked_tlb). False
  // leaves the access, and any fault, to Access().
  virtual bool TryFastLoad(uint32_t effective_address,
                           std::span<uint8_t> canonical_bytes,
                           CheckedTlb* checked_tlb) const noexcept {
    return false;
  }
  virtual bool TryFastStore(uint32_t effective_address,
                            std::span<const uint8_t> canonical_bytes,
                            CheckedTlb* checked_tlb) const noexcept {
    return false;
  }

  // The id this accessor's checked TLB entries carry when generated code may
  // probe them and access host_delta + ea itself, or 0. At most
  // kCheckedTlbMaxPackedAccessorId and never reused. An inline hit must equal
  // TryFastLoad/TryFastStore succeeding.
  virtual uint32_t inline_tlb_accessor_id() const noexcept { return 0; }
};

GuestMemoryAccessDiagnostic ValidateGuestMemoryAccessRequest(
    const GuestMemoryAccessRequest& request) noexcept;
bool GuestMemoryAccessCapabilitiesSupport(
    const GuestMemoryAccessCapabilities& capabilities,
    const GuestMemoryAccessRequest& request) noexcept;

// Validates the descriptor and hands the access to its accessor. There is no
// direct-memory fallback.
GuestMemoryAccessResult DispatchCheckedGuestMemoryAccess(
    const TranslationDomainDescriptor* descriptor,
    const GuestMemoryAccessRequest& request) noexcept;

}  // namespace xe::cpu

#endif  // XENIA_CPU_GUEST_MEMORY_ACCESS_H_

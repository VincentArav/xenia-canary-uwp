/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_address_space.h"

#include <array>
#include <limits>
#include <utility>

namespace xe::kernel {
namespace {

constexpr uint32_t kSmallPageSize = 0x00001000;
constexpr uint32_t kLargePageSize = 0x00010000;
constexpr uint32_t kDirectPageSize = 0x01000000;
constexpr uint32_t kLeafTableShift = 13;
constexpr uint32_t kLeafPteCountMask = 0x7FF;

static_assert(kUserModeCreateTimeFlagsOffset +
                  kUserModeAddressSpaceSegmentCount <=
              kUserModeControlPageSize);
static_assert(kUserModeFlushEntriesOffset +
                  kUserModeFlushMaximumEntryCount * sizeof(uint32_t) ==
              0xF00);

uint16_t ReadBe16(std::span<const uint8_t> bytes, size_t offset) {
  return uint16_t(uint16_t(bytes[offset]) << 8 | bytes[offset + 1]);
}

uint32_t ReadBe32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t(uint32_t(bytes[offset]) << 24 |
                  uint32_t(bytes[offset + 1]) << 16 |
                  uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3]);
}

bool IsValid(UserModePageTableKind kind) {
  switch (kind) {
    case UserModePageTableKind::k4KiB:
    case UserModePageTableKind::k4KiBKind1:
    case UserModePageTableKind::k64KiB:
    case UserModePageTableKind::k16MiB:
      return true;
    default:
      return false;
  }
}

bool IsValid(UserModeProblemStateKey key) {
  switch (key) {
    case UserModeProblemStateKey::kZero:
    case UserModeProblemStateKey::kOne:
      return true;
    default:
      return false;
  }
}

bool IsValid(UserModeMemoryAccess access) {
  switch (access) {
    case UserModeMemoryAccess::kRead:
    case UserModeMemoryAccess::kWrite:
    case UserModeMemoryAccess::kInstructionFetch:
      return true;
    default:
      return false;
  }
}

bool CheckedPhysicalRange(uint32_t base, uint32_t offset, uint32_t size,
                          uint32_t* address_out) {
  if (!size || offset > std::numeric_limits<uint32_t>::max() - base) {
    return false;
  }
  const uint32_t address = base + offset;
  if (size - 1 > std::numeric_limits<uint32_t>::max() - address) {
    return false;
  }
  *address_out = address;
  return true;
}

template <size_t Size>
bool ReadLiveBytes(uint32_t base, uint32_t offset,
                   const UserModeLivePhysicalReadCallback& read_live_physical,
                   UserModeAddressTranslationResult* result,
                   std::array<uint8_t, Size>* bytes_out) {
  uint32_t address;
  if (!CheckedPhysicalRange(base, offset, uint32_t(Size), &address)) {
    result->diagnostic =
        UserModeAddressTranslationDiagnostic::kBackingAddressOverflow;
    result->backing_failure_size = uint32_t(Size);
    return false;
  }

  if (!read_live_physical(address, std::span<uint8_t>(*bytes_out))) {
    result->diagnostic =
        UserModeAddressTranslationDiagnostic::kBackingReadFailed;
    result->backing_failure_physical_address = address;
    result->backing_failure_size = uint32_t(Size);
    result->backing_failure_address_is_exact = true;
    return false;
  }
  return true;
}

bool ReadLiveBe16(uint32_t base, uint32_t offset,
                  const UserModeLivePhysicalReadCallback& read_live_physical,
                  UserModeAddressTranslationResult* result, uint16_t* value_out,
                  uint32_t* address_out) {
  std::array<uint8_t, sizeof(uint16_t)> bytes{};
  if (!ReadLiveBytes(base, offset, read_live_physical, result, &bytes)) {
    return false;
  }
  *address_out = base + offset;
  *value_out = ReadBe16(bytes, 0);
  return true;
}

bool ReadLiveBe32(uint32_t base, uint32_t offset,
                  const UserModeLivePhysicalReadCallback& read_live_physical,
                  UserModeAddressTranslationResult* result, uint32_t* value_out,
                  uint32_t* address_out) {
  std::array<uint8_t, sizeof(uint32_t)> bytes{};
  if (!ReadLiveBytes(base, offset, read_live_physical, result, &bytes)) {
    return false;
  }
  *address_out = base + offset;
  *value_out = ReadBe32(bytes, 0);
  return true;
}

UserModePageProtection DecodeProtection(uint32_t effective_pte,
                                        UserModeProblemStateKey key) {
  const uint32_t page_protection = effective_pte & kUserModePteProtectionMask;
  if (key == UserModeProblemStateKey::kZero) {
    return page_protection == 3 ? UserModePageProtection::kReadOnly
                                : UserModePageProtection::kReadWrite;
  }

  switch (page_protection) {
    case 0:
      return UserModePageProtection::kNoAccess;
    case 2:
      return UserModePageProtection::kReadWrite;
    case 1:
    case 3:
    default:
      return UserModePageProtection::kReadOnly;
  }
}

bool IsProtectionPermitted(UserModePageProtection protection,
                           UserModeMemoryAccess access) {
  switch (access) {
    case UserModeMemoryAccess::kRead:
    case UserModeMemoryAccess::kInstructionFetch:
      return protection != UserModePageProtection::kNoAccess;
    case UserModeMemoryAccess::kWrite:
      return protection == UserModePageProtection::kReadWrite;
    default:
      return false;
  }
}

void FinishTranslation(UserModeAddressTranslationResult* result,
                       uint32_t physical_page_mask, uint32_t page_size) {
  result->physical_page_base = result->raw_pte & physical_page_mask;
  result->page_size = page_size;
  result->physical_address =
      result->physical_page_base |
      (result->effective_address & (result->page_size - 1));

  // Data accesses use PP=3 when the PTE's N bit is clear; instruction
  // fetches use the raw PP bits. The PTE itself is unchanged.
  result->effective_pte = result->raw_pte;
  if (result->requested_access != UserModeMemoryAccess::kInstructionFetch &&
      !(result->raw_pte & kUserModePteNoExecute)) {
    result->effective_pte |= kUserModePteProtectionMask;
  }

  result->pte_no_execute = (result->raw_pte & kUserModePteNoExecute) != 0;
  result->pte_guarded = (result->raw_pte & kUserModePteGuarded) != 0;
  result->pte_memory_coherent =
      (result->raw_pte & kUserModePteMemoryCoherent) != 0;
  result->pte_cache_inhibited =
      (result->raw_pte & kUserModePteCacheInhibited) != 0;
  result->pte_write_through = (result->raw_pte & kUserModePteWriteThrough) != 0;
  result->pte_changed = (result->raw_pte & kUserModePteChanged) != 0;
  result->pte_referenced = (result->raw_pte & kUserModePteReferenced) != 0;

  result->effective_protection =
      DecodeProtection(result->effective_pte, result->problem_state_key);
  if (!IsProtectionPermitted(result->effective_protection,
                             result->requested_access)) {
    result->access_faults |= UserModeAddressAccessFault::kProtection;
  }

  if (result->requested_access == UserModeMemoryAccess::kInstructionFetch) {
    if (result->create_time_segment.segment_no_execute ||
        result->pte_no_execute) {
      result->access_faults |= UserModeAddressAccessFault::kNoExecute;
    }
    if (result->pte_guarded) {
      result->access_faults |= UserModeAddressAccessFault::kGuardedInstruction;
    }
  }

  result->changed_bit_writeback_required =
      result->requested_access == UserModeMemoryAccess::kWrite &&
      !HasUserModeAddressAccessFault(result->access_faults,
                                     UserModeAddressAccessFault::kProtection) &&
      !result->pte_changed;
}

}  // namespace

UserModeCreateTimeLayoutDecodeResult DecodeUserModeCreateTimeAddressSpaceLayout(
    std::span<const uint8_t> control_page_snapshot) {
  UserModeCreateTimeLayoutDecodeResult result;
  if (control_page_snapshot.size() != kUserModeControlPageSize) {
    result.diagnostic =
        UserModeCreateTimeLayoutDiagnostic::kInvalidSnapshotSize;
    return result;
  }

  UserModeCreateTimeAddressSpaceLayout layout;
  for (size_t i = 0; i < layout.segments_.size(); ++i) {
    const uint8_t flags =
        control_page_snapshot[kUserModeCreateTimeFlagsOffset + i];
    const uint8_t kind_value =
        uint8_t((flags & 2 ? 2 : 0) | ((flags & 3) == 3 ? 1 : 0));
    UserModeCreateTimeSegmentLayout& segment = layout.segments_[i];
    segment.raw_create_flags = flags;
    segment.translation.page_table_kind =
        static_cast<UserModePageTableKind>(kind_value);
    segment.translation.segment_no_execute = (flags & 4) != 0;
    layout.packed_page_table_kind_summary_ |= uint32_t(kind_value) << (i * 2);
  }

  result.layout = layout;
  return result;
}

UserModeAddressTranslationResult ResolveUserModeLiveAddressForSegment(
    UserModeSegmentTranslationMode create_time_segment,
    uint32_t control_page_physical_address,
    UserModeProblemStateKey problem_state_key,
    UserModeMemoryAccess requested_access, uint32_t effective_address,
    const UserModeLivePhysicalReadCallback& read_live_physical) {
  UserModeAddressTranslationResult result;
  result.create_time_segment = create_time_segment;
  result.problem_state_key = problem_state_key;
  result.requested_access = requested_access;
  result.effective_address = effective_address;
  result.segment_index = effective_address >> 28;

  if (!IsValid(create_time_segment.page_table_kind) ||
      !IsValid(problem_state_key) || !IsValid(requested_access) ||
      !read_live_physical) {
    result.diagnostic = UserModeAddressTranslationDiagnostic::kInvalidArgument;
    return result;
  }

  switch (create_time_segment.page_table_kind) {
    case UserModePageTableKind::k4KiB:
    case UserModePageTableKind::k4KiBKind1: {
      const uint32_t root_offset =
          uint32_t(kUserModeSmallPageRootTableOffset) +
          sizeof(uint16_t) * ((effective_address >> 23) & 0x1FF);
      uint16_t root;
      if (!ReadLiveBe16(control_page_physical_address, root_offset,
                        read_live_physical, &result, &root,
                        &result.root_entry_physical_address)) {
        return result;
      }
      if (!root) {
        result.diagnostic =
            UserModeAddressTranslationDiagnostic::kRootNotPresent;
        return result;
      }

      const uint32_t leaf_physical_address = uint32_t(root) << kLeafTableShift;
      const uint32_t pte_offset =
          sizeof(uint32_t) * ((effective_address >> 12) & kLeafPteCountMask);
      if (!ReadLiveBe32(leaf_physical_address, pte_offset, read_live_physical,
                        &result, &result.raw_pte,
                        &result.pte_physical_address)) {
        return result;
      }
      if (!result.raw_pte) {
        result.diagnostic =
            UserModeAddressTranslationDiagnostic::kPageNotPresent;
        return result;
      }
      FinishTranslation(&result, 0xFFFFF000, kSmallPageSize);
      return result;
    }

    case UserModePageTableKind::k64KiB: {
      const uint32_t root_offset =
          uint32_t(kUserModeLargePageRootTableOffset) +
          sizeof(uint16_t) * ((effective_address >> 27) & 0x1F);
      uint16_t root;
      if (!ReadLiveBe16(control_page_physical_address, root_offset,
                        read_live_physical, &result, &root,
                        &result.root_entry_physical_address)) {
        return result;
      }
      if (!root) {
        result.diagnostic =
            UserModeAddressTranslationDiagnostic::kRootNotPresent;
        return result;
      }

      const uint32_t leaf_physical_address = uint32_t(root) << kLeafTableShift;
      const uint32_t pte_offset =
          sizeof(uint32_t) * ((effective_address >> 16) & kLeafPteCountMask);
      if (!ReadLiveBe32(leaf_physical_address, pte_offset, read_live_physical,
                        &result, &result.raw_pte,
                        &result.pte_physical_address)) {
        return result;
      }
      if (!result.raw_pte) {
        result.diagnostic =
            UserModeAddressTranslationDiagnostic::kPageNotPresent;
        return result;
      }
      FinishTranslation(&result, 0xFFFF0000, kLargePageSize);
      return result;
    }

    case UserModePageTableKind::k16MiB: {
      const uint32_t pte_offset =
          uint32_t(kUserModeDirectPageTableOffset) +
          sizeof(uint32_t) * ((effective_address >> 24) & 0xFF);
      if (!ReadLiveBe32(control_page_physical_address, pte_offset,
                        read_live_physical, &result, &result.raw_pte,
                        &result.pte_physical_address)) {
        return result;
      }
      if (!result.raw_pte) {
        result.diagnostic =
            UserModeAddressTranslationDiagnostic::kPageNotPresent;
        return result;
      }
      FinishTranslation(&result, 0xFF000000, kDirectPageSize);
      return result;
    }

    default:
      result.diagnostic =
          UserModeAddressTranslationDiagnostic::kInvalidArgument;
      return result;
  }
}

UserModeAddressTranslationResult ResolveUserModeLiveAddress(
    const UserModeCreateTimeAddressSpaceLayout& create_time_layout,
    uint32_t control_page_physical_address,
    UserModeProblemStateKey problem_state_key,
    UserModeMemoryAccess requested_access, uint32_t effective_address,
    const UserModeLivePhysicalReadCallback& read_live_physical) {
  return ResolveUserModeLiveAddressForSegment(
      create_time_layout.segment_for_effective_address(effective_address)
          .translation,
      control_page_physical_address, problem_state_key, requested_access,
      effective_address, read_live_physical);
}

UserModeFlushParseResult ParseUserModeLiveFlushRequest(
    std::span<const uint8_t> live_control_page_snapshot) {
  UserModeFlushParseResult result;
  if (live_control_page_snapshot.size() != kUserModeControlPageSize) {
    result.diagnostic = UserModeFlushParseDiagnostic::kInvalidSnapshotSize;
    return result;
  }

  result.raw_count =
      ReadBe32(live_control_page_snapshot, kUserModeFlushCountOffset);
  if (!result.raw_count || result.raw_count > kUserModeFlushMaximumEntryCount) {
    return result;
  }

  result.count_was_accepted = true;
  result.effective_addresses.reserve(result.raw_count);
  for (uint32_t i = 0; i < result.raw_count; ++i) {
    result.effective_addresses.push_back(
        ReadBe32(live_control_page_snapshot,
                 kUserModeFlushEntriesOffset + size_t(i) * sizeof(uint32_t)));
  }
  return result;
}

}  // namespace xe::kernel

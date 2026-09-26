/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_ADDRESS_SPACE_H_
#define XENIA_KERNEL_USER_MODE_ADDRESS_SPACE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "xenia/kernel/user_mode_control_page.h"

namespace xe::kernel {

inline constexpr size_t kUserModeAddressSpaceSegmentCount = 16;
inline constexpr size_t kUserModeCreateTimeFlagsOffset = 0x840;
inline constexpr size_t kUserModeSmallPageRootTableOffset = 0x000;
inline constexpr size_t kUserModeDirectPageTableOffset = 0x400;
inline constexpr size_t kUserModeLargePageRootTableOffset = 0x800;
inline constexpr size_t kUserModeFlushCountOffset = 0x880;
inline constexpr size_t kUserModeFlushEntriesOffset = 0x884;
inline constexpr uint32_t kUserModeFlushMaximumEntryCount = 0x19F;

inline constexpr uint32_t kUserModePteProtectionMask = 0x003;
inline constexpr uint32_t kUserModePteNoExecute = 0x004;
inline constexpr uint32_t kUserModePteGuarded = 0x008;
inline constexpr uint32_t kUserModePteMemoryCoherent = 0x010;
inline constexpr uint32_t kUserModePteCacheInhibited = 0x020;
inline constexpr uint32_t kUserModePteWriteThrough = 0x040;
inline constexpr uint32_t kUserModePteChanged = 0x080;
inline constexpr uint32_t kUserModePteReferenced = 0x100;

// A segment's two-bit page table kind. Create emits 0, 2 and 3; kind 1 is
// treated as 0.
enum class UserModePageTableKind : uint8_t {
  k4KiB = 0,
  k4KiBKind1 = 1,
  k64KiB = 2,
  k16MiB = 3,
};

struct UserModeSegmentTranslationMode {
  UserModePageTableKind page_table_kind = UserModePageTableKind::k4KiB;
  bool segment_no_execute = false;
};

// A segment's flags byte (control page +0x840), decoded at Create. Roots and
// PTEs are read live.
struct UserModeCreateTimeSegmentLayout {
  uint8_t raw_create_flags = 0;
  UserModeSegmentTranslationMode translation;
};

struct UserModeCreateTimeLayoutDecodeResult;

class UserModeCreateTimeAddressSpaceLayout {
 public:
  UserModeCreateTimeAddressSpaceLayout(
      const UserModeCreateTimeAddressSpaceLayout&) = default;
  UserModeCreateTimeAddressSpaceLayout& operator=(
      const UserModeCreateTimeAddressSpaceLayout&) = default;
  UserModeCreateTimeAddressSpaceLayout(UserModeCreateTimeAddressSpaceLayout&&) =
      default;
  UserModeCreateTimeAddressSpaceLayout& operator=(
      UserModeCreateTimeAddressSpaceLayout&&) = default;

  const std::array<UserModeCreateTimeSegmentLayout,
                   kUserModeAddressSpaceSegmentCount>&
  segments() const {
    return segments_;
  }
  const UserModeCreateTimeSegmentLayout& segment_for_effective_address(
      uint32_t effective_address) const {
    return segments_[effective_address >> 28];
  }
  uint32_t packed_page_table_kind_summary() const {
    return packed_page_table_kind_summary_;
  }

 private:
  friend UserModeCreateTimeLayoutDecodeResult
  DecodeUserModeCreateTimeAddressSpaceLayout(
      std::span<const uint8_t> control_page_snapshot);

  UserModeCreateTimeAddressSpaceLayout() = default;

  std::array<UserModeCreateTimeSegmentLayout, kUserModeAddressSpaceSegmentCount>
      segments_{};
  uint32_t packed_page_table_kind_summary_ = 0;
};

enum class UserModeCreateTimeLayoutDiagnostic {
  kNone,
  kInvalidSnapshotSize,
};

struct UserModeCreateTimeLayoutDecodeResult {
  UserModeCreateTimeLayoutDiagnostic diagnostic =
      UserModeCreateTimeLayoutDiagnostic::kNone;
  std::optional<UserModeCreateTimeAddressSpaceLayout> layout;

  explicit operator bool() const {
    return diagnostic == UserModeCreateTimeLayoutDiagnostic::kNone &&
           layout.has_value();
  }
};

// Decodes the 16 segment flag bytes (+0x840..+0x84F) of a control-page
// snapshot.
UserModeCreateTimeLayoutDecodeResult DecodeUserModeCreateTimeAddressSpaceLayout(
    std::span<const uint8_t> control_page_snapshot);

enum class UserModeProblemStateKey : uint8_t {
  kZero = 0,
  kOne = 1,
};

enum class UserModeMemoryAccess : uint8_t {
  kRead,
  kWrite,
  kInstructionFetch,
};

enum class UserModePageProtection : uint8_t {
  kNoAccess,
  kReadOnly,
  kReadWrite,
};

enum class UserModeAddressAccessFault : uint32_t {
  kNone = 0,
  kProtection = 1u << 0,
  kNoExecute = 1u << 1,
  kGuardedInstruction = 1u << 2,
};

constexpr UserModeAddressAccessFault operator|(
    UserModeAddressAccessFault left, UserModeAddressAccessFault right) {
  return static_cast<UserModeAddressAccessFault>(static_cast<uint32_t>(left) |
                                                 static_cast<uint32_t>(right));
}

constexpr UserModeAddressAccessFault& operator|=(
    UserModeAddressAccessFault& left, UserModeAddressAccessFault right) {
  left = left | right;
  return left;
}

constexpr bool HasUserModeAddressAccessFault(UserModeAddressAccessFault faults,
                                             UserModeAddressAccessFault fault) {
  return (static_cast<uint32_t>(faults) & static_cast<uint32_t>(fault)) != 0;
}

enum class UserModeAddressTranslationDiagnostic {
  kNone,
  kInvalidArgument,
  kBackingAddressOverflow,
  kBackingReadFailed,
  kRootNotPresent,
  kPageNotPresent,
};

// Copies destination.size() bytes of guest physical memory, or returns false.
using UserModeLivePhysicalReadCallback = std::function<bool(
    uint32_t physical_address, std::span<uint8_t> destination)>;

struct UserModeAddressTranslationResult {
  UserModeAddressTranslationDiagnostic diagnostic =
      UserModeAddressTranslationDiagnostic::kNone;
  UserModeAddressAccessFault access_faults = UserModeAddressAccessFault::kNone;
  UserModeMemoryAccess requested_access = UserModeMemoryAccess::kRead;
  UserModeProblemStateKey problem_state_key = UserModeProblemStateKey::kZero;
  UserModeSegmentTranslationMode create_time_segment;
  uint32_t effective_address = 0;
  uint32_t segment_index = 0;

  uint32_t root_entry_physical_address = 0;
  uint32_t pte_physical_address = 0;
  uint32_t raw_pte = 0;

  // raw_pte for instruction fetches; data accesses use PP=3 for an N-clear
  // PTE.
  uint32_t effective_pte = 0;

  uint32_t physical_address = 0;
  uint32_t physical_page_base = 0;
  uint32_t page_size = 0;
  UserModePageProtection effective_protection =
      UserModePageProtection::kNoAccess;

  bool pte_no_execute = false;
  bool pte_guarded = false;
  bool pte_memory_coherent = false;
  bool pte_cache_inhibited = false;
  bool pte_write_through = false;
  bool pte_changed = false;
  bool pte_referenced = false;

  // Set when a permitted write sees C clear; the caller sets it (PTE | 0x80).
  bool changed_bit_writeback_required = false;

  // The physical address whose read failed; 0, with address_is_exact false,
  // when the range overflowed.
  uint32_t backing_failure_physical_address = 0;
  uint32_t backing_failure_size = 0;
  bool backing_failure_address_is_exact = false;

  bool translated() const {
    return diagnostic == UserModeAddressTranslationDiagnostic::kNone;
  }
  bool access_permitted() const {
    return translated() && access_faults == UserModeAddressAccessFault::kNone;
  }
};

// Resolves an EA in a segment of the given kind. control_page_physical_address
// is nonzero, 4 KiB-aligned and below 0x20000000.
UserModeAddressTranslationResult ResolveUserModeLiveAddressForSegment(
    UserModeSegmentTranslationMode create_time_segment,
    uint32_t control_page_physical_address,
    UserModeProblemStateKey problem_state_key,
    UserModeMemoryAccess requested_access, uint32_t effective_address,
    const UserModeLivePhysicalReadCallback& read_live_physical);

// Resolves an EA with the segment kind and N bit of the Create-time layout.
UserModeAddressTranslationResult ResolveUserModeLiveAddress(
    const UserModeCreateTimeAddressSpaceLayout& create_time_layout,
    uint32_t control_page_physical_address,
    UserModeProblemStateKey problem_state_key,
    UserModeMemoryAccess requested_access, uint32_t effective_address,
    const UserModeLivePhysicalReadCallback& read_live_physical);

enum class UserModeFlushParseDiagnostic {
  kNone,
  kInvalidSnapshotSize,
};

struct UserModeFlushParseResult {
  UserModeFlushParseDiagnostic diagnostic = UserModeFlushParseDiagnostic::kNone;
  uint32_t raw_count = 0;
  bool count_was_accepted = false;
  std::vector<uint32_t> effective_addresses;

  explicit operator bool() const {
    return diagnostic == UserModeFlushParseDiagnostic::kNone;
  }
};

// Parses a TB flush request from a control-page snapshot. Counts 1..0x19F are
// accepted; other counts are no-ops.
UserModeFlushParseResult ParseUserModeLiveFlushRequest(
    std::span<const uint8_t> live_control_page_snapshot);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_ADDRESS_SPACE_H_

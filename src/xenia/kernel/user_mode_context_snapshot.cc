/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_context_snapshot.h"

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

bool IsSupportedContextHeap(HeapType heap_type) {
  switch (heap_type) {
    case HeapType::kGuestVirtual:
    case HeapType::kGuestXex:
    case HeapType::kGuestPhysical:
      return true;
    case HeapType::kHostPhysical:
    default:
      return false;
  }
}

struct ValidatedContextRange {
  BaseHeap* heap = nullptr;
  uint32_t end = 0;
};

UserModeContextSnapshotDiagnostic ValidateContextRangeLocked(
    Memory& memory, uint32_t context_virtual_address, bool require_write,
    ValidatedContextRange* range_out) {
  const uint64_t context_end_64 =
      uint64_t(context_virtual_address) + kUserModeContextMinimumSize - 1;
  if (context_end_64 > std::numeric_limits<uint32_t>::max()) {
    return UserModeContextSnapshotDiagnostic::kAddressRangeWrap;
  }
  const uint32_t context_end = static_cast<uint32_t>(context_end_64);
  range_out->end = context_end;

  BaseHeap* const start_heap = memory.LookupHeap(context_virtual_address);
  BaseHeap* const end_heap = memory.LookupHeap(context_end);
  if (!start_heap || !end_heap) {
    return UserModeContextSnapshotDiagnostic::kUnmappedRange;
  }
  if (start_heap != end_heap) {
    return UserModeContextSnapshotDiagnostic::kCrossesHeapBoundary;
  }
  const uint64_t heap_base = start_heap->heap_base();
  const uint64_t heap_end_exclusive = heap_base + start_heap->heap_size();
  if (context_virtual_address < heap_base ||
      context_end_64 >= heap_end_exclusive || !start_heap->page_size()) {
    return UserModeContextSnapshotDiagnostic::kUnmappedRange;
  }
  if (!IsSupportedContextHeap(start_heap->heap_type())) {
    return UserModeContextSnapshotDiagnostic::kUnsupportedMapping;
  }

  if (memory.OverlapsVirtualMappedRange(
          context_virtual_address, static_cast<uint32_t>(context_end_64))) {
    return UserModeContextSnapshotDiagnostic::kUnsupportedMapping;
  }

  const uint64_t page_size = start_heap->page_size();
  const uint64_t first_page =
      heap_base +
      ((uint64_t(context_virtual_address) - heap_base) / page_size) * page_size;
  for (uint64_t page = first_page; page <= context_end_64; page += page_size) {
    const std::optional<HeapPageInfo> page_info =
        start_heap->QueryPageInfo(static_cast<uint32_t>(page));
    if (!page_info || !(page_info->state & kMemoryAllocationCommit)) {
      return UserModeContextSnapshotDiagnostic::kRangeNotCommitted;
    }
    if (!(page_info->protect & kMemoryProtectRead)) {
      return UserModeContextSnapshotDiagnostic::kRangeNotReadable;
    }
    if (require_write && !(page_info->protect & (kMemoryProtectWrite |
                                                 kMemoryProtectWriteCombine))) {
      return UserModeContextSnapshotDiagnostic::kRangeNotWritable;
    }
  }

  range_out->heap = start_heap;
  return UserModeContextSnapshotDiagnostic::kNone;
}

}  // namespace

UserModeContextSnapshotResult ResolveUserModeContextSnapshot(
    Memory& memory, uint32_t context_virtual_address) {
  UserModeContextSnapshotResult result;
  result.context_virtual_address = context_virtual_address;

  static_assert(kUserModeContextMinimumSize != 0);

  std::array<uint8_t, kUserModeContextMinimumSize> snapshot_bytes{};
  {
    // Hold the global lock, which guards allocation and protection changes,
    // from validation through the copy.
    auto global_lock = global_critical_region::AcquireDirect();

    ValidatedContextRange range;
    result.diagnostic = ValidateContextRangeLocked(
        memory, context_virtual_address, false, &range);
    result.context_end_virtual_address = range.end;
    if (result.diagnostic != UserModeContextSnapshotDiagnostic::kNone) {
      return result;
    }

    std::memcpy(
        snapshot_bytes.data(),
        memory.TranslateVirtual<const uint8_t*>(context_virtual_address),
        snapshot_bytes.size());
  }

  UserModeIntegerContextResult decoded =
      DecodeUserModeIntegerContext(snapshot_bytes);
  if (!decoded) {
    result.diagnostic = UserModeContextSnapshotDiagnostic::kDecodeFailed;
    return result;
  }
  result.context = std::move(decoded.context);
  return result;
}

UserModeContextWriteResult WriteUserModeIntegerContextSnapshot(
    Memory& memory, uint32_t context_virtual_address,
    const UserModeIntegerContext& context) {
  UserModeContextWriteResult result;
  result.context_virtual_address = context_virtual_address;

  std::array<uint8_t, kUserModeContextMinimumSize> snapshot_bytes{};
  {
    auto global_lock = global_critical_region::AcquireDirect();
    ValidatedContextRange range;
    result.diagnostic = ValidateContextRangeLocked(
        memory, context_virtual_address, true, &range);
    result.context_end_virtual_address = range.end;
    if (result.diagnostic != UserModeContextSnapshotDiagnostic::kNone) {
      return result;
    }

    std::memcpy(
        snapshot_bytes.data(),
        memory.TranslateVirtual<const uint8_t*>(context_virtual_address),
        snapshot_bytes.size());
    if (EncodeUserModeIntegerContext(context, snapshot_bytes) !=
        UserModeContextCodecError::kNone) {
      result.diagnostic = UserModeContextSnapshotDiagnostic::kDecodeFailed;
      return result;
    }
    std::memcpy(memory.TranslateVirtual<uint8_t*>(context_virtual_address),
                snapshot_bytes.data(), snapshot_bytes.size());
  }
  return result;
}

std::string_view UserModeContextSnapshotDiagnosticToString(
    UserModeContextSnapshotDiagnostic diagnostic) {
  switch (diagnostic) {
    case UserModeContextSnapshotDiagnostic::kNone:
      return "none";
    case UserModeContextSnapshotDiagnostic::kAddressRangeWrap:
      return "the user-mode context range wraps the guest address space";
    case UserModeContextSnapshotDiagnostic::kUnmappedRange:
      return "the user-mode context range is outside mapped guest heaps";
    case UserModeContextSnapshotDiagnostic::kCrossesHeapBoundary:
      return "the user-mode context range crosses guest heap boundaries";
    case UserModeContextSnapshotDiagnostic::kUnsupportedMapping:
      return "the user-mode context uses an unsupported memory mapping";
    case UserModeContextSnapshotDiagnostic::kRangeNotCommitted:
      return "the complete user-mode context range is not committed";
    case UserModeContextSnapshotDiagnostic::kRangeNotReadable:
      return "the complete user-mode context range is not readable";
    case UserModeContextSnapshotDiagnostic::kRangeNotWritable:
      return "the complete user-mode context range is not writable";
    case UserModeContextSnapshotDiagnostic::kDecodeFailed:
      return "the user-mode context snapshot could not be decoded";
  }
  return "unknown user-mode context snapshot diagnostic";
}

}  // namespace xe::kernel

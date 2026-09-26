/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_control_page.h"

#include <cstdint>
#include <limits>

#include "xenia/memory.h"

namespace xe::kernel {
namespace {

constexpr uint32_t kUserModeControlPageAlignmentMask =
    kUserModeControlPageSize - 1;
constexpr uint32_t kUserModePhysicalAddressLimit = 0x20000000;

}  // namespace

UserModeControlPageResolution ResolveUserModeControlPage(
    Memory& memory, uint32_t control_page_virtual_address) {
  UserModeControlPageResolution result;
  result.control_page_virtual_address = control_page_virtual_address;

  // The control page is one aligned 4 KiB page; check the address before
  // touching memory.
  if (control_page_virtual_address == 0 ||
      (control_page_virtual_address & kUserModeControlPageAlignmentMask) != 0) {
    result.diagnostic = UserModeControlPageDiagnostic::kInvalidVirtualAddress;
    return result;
  }

  const uint64_t page_end_64 =
      uint64_t(control_page_virtual_address) + kUserModeControlPageSize - 1;
  if (page_end_64 > std::numeric_limits<uint32_t>::max()) {
    result.diagnostic = UserModeControlPageDiagnostic::kInvalidVirtualRange;
    return result;
  }
  const uint32_t page_end = static_cast<uint32_t>(page_end_64);

  BaseHeap* const heap = memory.LookupHeap(control_page_virtual_address);
  if (!heap || memory.LookupHeap(page_end) != heap) {
    result.diagnostic = UserModeControlPageDiagnostic::kInvalidVirtualRange;
    return result;
  }

  const uint64_t heap_end_exclusive =
      uint64_t(heap->heap_base()) + heap->heap_size();
  if (control_page_virtual_address < heap->heap_base() ||
      page_end_64 >= heap_end_exclusive) {
    result.diagnostic = UserModeControlPageDiagnostic::kInvalidVirtualRange;
    return result;
  }

  // Query commitment and protection together, under the heap lock.
  HeapAllocationInfo page_info{};
  if (!heap->QueryRegionInfo(control_page_virtual_address, &page_info) ||
      page_info.region_size < kUserModeControlPageSize ||
      !(page_info.state & kMemoryAllocationCommit)) {
    result.diagnostic = UserModeControlPageDiagnostic::kPageNotCommitted;
    return result;
  }

  const bool is_readable = (page_info.protect & kMemoryProtectRead) != 0;
  const bool is_writable =
      (page_info.protect &
       (kMemoryProtectWrite | kMemoryProtectWriteCombine)) != 0;
  if (!is_readable || !is_writable) {
    result.diagnostic = UserModeControlPageDiagnostic::kPageNotReadWrite;
    return result;
  }

  // Only the physical-view heaps map a virtual address to physical memory.
  if (heap->heap_type() != HeapType::kGuestPhysical) {
    result.diagnostic = UserModeControlPageDiagnostic::kUnsupportedMapping;
    return result;
  }

  const uint32_t physical_address =
      memory.GetPhysicalAddress(control_page_virtual_address);
  if (physical_address == 0 ||
      physical_address == std::numeric_limits<uint32_t>::max() ||
      (physical_address & kUserModeControlPageAlignmentMask) != 0 ||
      physical_address >= kUserModePhysicalAddressLimit) {
    result.diagnostic = UserModeControlPageDiagnostic::kInvalidPhysicalAddress;
    return result;
  }

  result.control_page_physical_address = physical_address;
  return result;
}

}  // namespace xe::kernel

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_physical_memory.h"

#include <atomic>
#include <cstring>
#include <limits>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

constexpr uint32_t kUserModePhysicalAddressLimit = 0x20000000;
constexpr uint32_t kUserModePhysicalPageSize = 0x1000;

}  // namespace

UserModePhysicalReadTransaction::UserModePhysicalReadTransaction(Memory& memory)
    : memory_(memory),
      global_lock_(xe::global_critical_region::AcquireDirect()) {
  const BaseHeap* const physical_heap = memory_.GetPhysicalHeap();
  physical_heap_is_valid_ =
      physical_heap && physical_heap->heap_base() == 0 &&
      physical_heap->heap_size() == kUserModePhysicalAddressLimit &&
      physical_heap->page_size() == kUserModePhysicalPageSize &&
      physical_heap->heap_type() == HeapType::kGuestPhysical;
}

UserModePhysicalReadResult UserModePhysicalReadTransaction::TryRead(
    uint32_t physical_address, std::span<uint8_t> destination) const {
  UserModePhysicalReadResult result;
  result.requested_physical_address = physical_address;
  result.requested_size = destination.size();

  if (destination.empty() || !destination.data() ||
      destination.size() > std::numeric_limits<uint32_t>::max()) {
    result.diagnostic = UserModePhysicalReadDiagnostic::kInvalidSize;
    return result;
  }
  if (destination.size() - 1 >
      size_t(std::numeric_limits<uint32_t>::max() - physical_address)) {
    result.diagnostic = UserModePhysicalReadDiagnostic::kAddressOverflow;
    return result;
  }

  const uint32_t last_address =
      physical_address + uint32_t(destination.size() - 1);
  if (physical_address >= kUserModePhysicalAddressLimit ||
      last_address >= kUserModePhysicalAddressLimit) {
    result.diagnostic = UserModePhysicalReadDiagnostic::kOutsidePhysicalMemory;
    return result;
  }
  if (!physical_heap_is_valid_) {
    result.diagnostic =
        UserModePhysicalReadDiagnostic::kUnexpectedPhysicalHeapLayout;
    return result;
  }

  // All arithmetic below is safe after the complete inclusive range check.
  const uint32_t first_page =
      physical_address & ~(kUserModePhysicalPageSize - 1);
  const uint32_t last_page = last_address & ~(kUserModePhysicalPageSize - 1);
  BaseHeap* const physical_heap = memory_.GetPhysicalHeap();
  for (uint32_t page = first_page;; page += kUserModePhysicalPageSize) {
    const std::optional<HeapPageInfo> page_info =
        physical_heap->QueryPageInfo(page);
    if (!page_info.has_value()) {
      result.diagnostic = UserModePhysicalReadDiagnostic::kPageQueryFailed;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->state & kMemoryAllocationReserve)) {
      result.diagnostic = UserModePhysicalReadDiagnostic::kPageNotReserved;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->state & kMemoryAllocationCommit)) {
      result.diagnostic = UserModePhysicalReadDiagnostic::kPageNotCommitted;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->protect & kMemoryProtectRead)) {
      result.diagnostic = UserModePhysicalReadDiagnostic::kPageNotReadable;
      result.failing_page_physical_address = page;
      return result;
    }
    if (page == last_page) {
      break;
    }
  }

  // Copy only after the whole range validated.
  std::memcpy(destination.data(),
              memory_.TranslatePhysical<const uint8_t*>(physical_address),
              destination.size());
  return result;
}

UserModePhysicalWriteProbeResult UserModePhysicalReadTransaction::ProbeWrite(
    uint32_t physical_address, size_t size) const {
  UserModePhysicalWriteProbeResult result;
  result.requested_physical_address = physical_address;
  result.requested_size = size;

  if (!size || size > std::numeric_limits<uint32_t>::max()) {
    result.diagnostic = UserModePhysicalWriteProbeDiagnostic::kInvalidSize;
    return result;
  }
  if (size - 1 >
      size_t(std::numeric_limits<uint32_t>::max() - physical_address)) {
    result.diagnostic = UserModePhysicalWriteProbeDiagnostic::kAddressOverflow;
    return result;
  }

  const uint32_t last_address = physical_address + uint32_t(size - 1);
  if (physical_address >= kUserModePhysicalAddressLimit ||
      last_address >= kUserModePhysicalAddressLimit) {
    result.diagnostic =
        UserModePhysicalWriteProbeDiagnostic::kOutsidePhysicalMemory;
    return result;
  }
  if (!physical_heap_is_valid_) {
    result.diagnostic =
        UserModePhysicalWriteProbeDiagnostic::kUnexpectedPhysicalHeapLayout;
    return result;
  }

  const uint32_t first_page =
      physical_address & ~(kUserModePhysicalPageSize - 1);
  const uint32_t last_page = last_address & ~(kUserModePhysicalPageSize - 1);
  BaseHeap* const physical_heap = memory_.GetPhysicalHeap();
  for (uint32_t page = first_page;; page += kUserModePhysicalPageSize) {
    const std::optional<HeapPageInfo> page_info =
        physical_heap->QueryPageInfo(page);
    if (!page_info.has_value()) {
      result.diagnostic =
          UserModePhysicalWriteProbeDiagnostic::kPageQueryFailed;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->state & kMemoryAllocationReserve)) {
      result.diagnostic =
          UserModePhysicalWriteProbeDiagnostic::kPageNotReserved;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->state & kMemoryAllocationCommit)) {
      result.diagnostic =
          UserModePhysicalWriteProbeDiagnostic::kPageNotCommitted;
      result.failing_page_physical_address = page;
      return result;
    }
    if (!(page_info->protect & kMemoryProtectRead) ||
        !(page_info->protect &
          (kMemoryProtectWrite | kMemoryProtectWriteCombine))) {
      result.diagnostic =
          UserModePhysicalWriteProbeDiagnostic::kPageNotWritable;
      result.failing_page_physical_address = page;
      return result;
    }
    if (page == last_page) {
      break;
    }
  }
  return result;
}

bool UserModePhysicalReadTransaction::NotifyWriteInvalidationCallbacks(
    uint32_t physical_address, size_t size) const {
  if (size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return memory_.NotifyPhysicalMemoryWriteInvalidationCallbacksLocked(
      global_lock_, physical_address, static_cast<uint32_t>(size));
}

bool UserModePhysicalReadTransaction::CompareExchangeBe32Prevalidated(
    uint32_t physical_address, uint32_t expected_raw_value,
    uint32_t desired_raw_value, uint32_t* observed_raw_value) const {
  assert_zero(physical_address & (alignof(uint32_t) - 1));
  assert_not_null(observed_raw_value);
  xe::be<uint32_t> expected(expected_raw_value);
  const xe::be<uint32_t> desired(desired_raw_value);
  uint32_t* const physical_word =
      memory_.TranslatePhysical<uint32_t*>(physical_address);
  std::atomic_ref<uint32_t> atomic_physical_word(*physical_word);
  const bool exchanged = atomic_physical_word.compare_exchange_strong(
      expected.value, desired.value, std::memory_order_acq_rel,
      std::memory_order_acquire);
  xe::be<uint32_t> observed;
  observed.value = expected.value;
  *observed_raw_value = observed.get();
  return exchanged;
}

bool UserModePhysicalReadTransaction::CompareExchangeRaw32Prevalidated(
    uint32_t physical_address, uint32_t expected_value, uint32_t desired_value,
    uint32_t* observed_value) const {
  assert_zero(physical_address & (alignof(uint32_t) - 1));
  assert_not_null(observed_value);
  uint32_t* const physical_word =
      memory_.TranslatePhysical<uint32_t*>(physical_address);
  std::atomic_ref<uint32_t> atomic_physical_word(*physical_word);
  const bool exchanged = atomic_physical_word.compare_exchange_strong(
      expected_value, desired_value, std::memory_order_acq_rel,
      std::memory_order_acquire);
  *observed_value = expected_value;
  return exchanged;
}

bool UserModePhysicalReadTransaction::CompareExchangeRaw64Prevalidated(
    uint32_t physical_address, uint64_t expected_value, uint64_t desired_value,
    uint64_t* observed_value) const {
  assert_zero(physical_address & (alignof(uint64_t) - 1));
  assert_not_null(observed_value);
  uint64_t* const physical_word =
      memory_.TranslatePhysical<uint64_t*>(physical_address);
  std::atomic_ref<uint64_t> atomic_physical_word(*physical_word);
  const bool exchanged = atomic_physical_word.compare_exchange_strong(
      expected_value, desired_value, std::memory_order_acq_rel,
      std::memory_order_acquire);
  *observed_value = expected_value;
  return exchanged;
}

void UserModePhysicalReadTransaction::CopyWritePrevalidated(
    uint32_t physical_address, std::span<const uint8_t> source) const {
  assert_false(source.empty());
  assert_not_null(source.data());
  std::memcpy(memory_.TranslatePhysical<uint8_t*>(physical_address),
              source.data(), source.size());
}

}  // namespace xe::kernel

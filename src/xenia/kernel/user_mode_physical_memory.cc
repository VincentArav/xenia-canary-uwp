/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_physical_memory.h"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

constexpr uint32_t kUserModePhysicalAddressLimit = 0x20000000;
constexpr uint32_t kUserModePhysicalPageSize = 0x1000;

// Physical pages this thread found committed and readable, valid until the
// translation clear epoch advances (on every physical heap change, and XEX
// heap change while the image window is open). Image window pages are numbered
// from 0x40000, apart from physical ones.
struct ValidatedReadPages {
  const Memory* memory = nullptr;
  uint64_t clear_epoch = 0;
  // Page number + 1, direct-mapped; 0 is empty.
  std::array<uint32_t, 1024> pages{};
};
thread_local ValidatedReadPages validated_read_pages;

bool IsValidatedReadPage(const Memory& memory, uint32_t page) {
  const ValidatedReadPages& validated = validated_read_pages;
  const uint32_t page_number = page / kUserModePhysicalPageSize;
  return validated.memory == &memory &&
         validated.clear_epoch == Memory::translation_clear_epoch() &&
         validated.pages[page_number % validated.pages.size()] ==
             page_number + 1;
}

// Under the global lock, so no heap change is in progress.
void MarkValidatedReadPage(const Memory& memory, uint32_t page) {
  ValidatedReadPages& validated = validated_read_pages;
  const uint64_t clear_epoch = Memory::translation_clear_epoch();
  if (validated.memory != &memory || validated.clear_epoch != clear_epoch) {
    validated.pages.fill(0);
    validated.memory = &memory;
    validated.clear_epoch = clear_epoch;
  }
  const uint32_t page_number = page / kUserModePhysicalPageSize;
  validated.pages[page_number % validated.pages.size()] = page_number + 1;
}

// Whether [first, last] lies wholly inside the image window while it is open.
bool IsImageWindowRange(const Memory& memory, uint32_t first, uint32_t last) {
  return IsImagePhysicalWindowAddress(first) &&
         IsImagePhysicalWindowAddress(last) &&
         memory.image_physical_window_enabled();
}

// The state of the XEX image page an image window page names, checked with the
// image heap's own page size and protection.
std::optional<HeapPageInfo> QueryImageWindowPage(const Memory& memory,
                                                 uint32_t page) {
  const uint32_t image_address = ImageVirtualAddressFromWindow(page);
  const BaseHeap* const heap = memory.LookupHeap(image_address);
  if (!heap || heap->heap_type() != HeapType::kGuestXex) {
    return std::nullopt;
  }
  return heap->QueryPageInfo(image_address);
}

}  // namespace

uint8_t* TranslateUserModePhysical(const Memory& memory,
                                   uint32_t physical_address) {
  return IsImagePhysicalWindowAddress(physical_address)
             ? memory.TranslateVirtual<uint8_t*>(
                   ImageVirtualAddressFromWindow(physical_address))
             : memory.TranslatePhysical<uint8_t*>(physical_address);
}

UserModePhysicalReadTransaction::UserModePhysicalReadTransaction(Memory& memory)
    : memory_(memory),
      global_lock_(xe::global_critical_region::AcquireDeferred()) {
  const BaseHeap* const physical_heap = memory_.GetPhysicalHeap();
  physical_heap_is_valid_ =
      physical_heap && physical_heap->heap_base() == 0 &&
      physical_heap->heap_size() == kUserModePhysicalAddressLimit &&
      physical_heap->page_size() == kUserModePhysicalPageSize &&
      physical_heap->heap_type() == HeapType::kGuestPhysical;
}

void UserModePhysicalReadTransaction::EnsureLocked() const {
  if (!global_lock_.owns_lock()) {
    global_lock_.lock();
  }
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
  // Above physical memory, only a range wholly inside the open image window.
  const bool image_window = last_address >= kUserModePhysicalAddressLimit;
  if (image_window &&
      !IsImageWindowRange(memory_, physical_address, last_address)) {
    result.diagnostic = UserModePhysicalReadDiagnostic::kOutsidePhysicalMemory;
    return result;
  }
  if (!image_window && !physical_heap_is_valid_) {
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
    if (IsValidatedReadPage(memory_, page)) {
      if (page == last_page) {
        break;
      }
      continue;
    }
    EnsureLocked();
    const std::optional<HeapPageInfo> page_info =
        image_window ? QueryImageWindowPage(memory_, page)
                     : physical_heap->QueryPageInfo(page);
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
    MarkValidatedReadPage(memory_, page);
    if (page == last_page) {
      break;
    }
  }

  // Copy only after the whole range validated.
  std::memcpy(destination.data(),
              TranslateUserModePhysical(memory_, physical_address),
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
  // Above physical memory, only a range wholly inside the open image window.
  const bool image_window = last_address >= kUserModePhysicalAddressLimit;
  if (image_window &&
      !IsImageWindowRange(memory_, physical_address, last_address)) {
    result.diagnostic =
        UserModePhysicalWriteProbeDiagnostic::kOutsidePhysicalMemory;
    return result;
  }
  if (!image_window && !physical_heap_is_valid_) {
    result.diagnostic =
        UserModePhysicalWriteProbeDiagnostic::kUnexpectedPhysicalHeapLayout;
    return result;
  }

  EnsureLocked();
  const uint32_t first_page =
      physical_address & ~(kUserModePhysicalPageSize - 1);
  const uint32_t last_page = last_address & ~(kUserModePhysicalPageSize - 1);
  BaseHeap* const physical_heap = memory_.GetPhysicalHeap();
  for (uint32_t page = first_page;; page += kUserModePhysicalPageSize) {
    const std::optional<HeapPageInfo> page_info =
        image_window ? QueryImageWindowPage(memory_, page)
                     : physical_heap->QueryPageInfo(page);
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
    // XEX image pages are never WriteCombine, and image code and read-only
    // data pages are Read only.
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
  EnsureLocked();
  // Only physical memory has observers (GPU and XMA caches), and nothing
  // caches image bytes, so an image window range needs no dispatch.
  if (IsImagePhysicalWindowAddress(physical_address)) {
    return size != 0 &&
           size - 1 < kImagePhysicalWindowBase + kImagePhysicalWindowSize -
                          physical_address &&
           memory_.image_physical_window_enabled();
  }
  return memory_.NotifyPhysicalMemoryWriteInvalidationCallbacksLocked(
      global_lock_, physical_address, static_cast<uint32_t>(size));
}

bool UserModePhysicalReadTransaction::CompareExchangeBe32Prevalidated(
    uint32_t physical_address, uint32_t expected_raw_value,
    uint32_t desired_raw_value, uint32_t* observed_raw_value) const {
  EnsureLocked();
  assert_zero(physical_address & (alignof(uint32_t) - 1));
  assert_not_null(observed_raw_value);
  xe::be<uint32_t> expected(expected_raw_value);
  const xe::be<uint32_t> desired(desired_raw_value);
  uint32_t* const physical_word = reinterpret_cast<uint32_t*>(
      TranslateUserModePhysical(memory_, physical_address));
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
  EnsureLocked();
  assert_zero(physical_address & (alignof(uint32_t) - 1));
  assert_not_null(observed_value);
  uint32_t* const physical_word = reinterpret_cast<uint32_t*>(
      TranslateUserModePhysical(memory_, physical_address));
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
  EnsureLocked();
  assert_zero(physical_address & (alignof(uint64_t) - 1));
  assert_not_null(observed_value);
  uint64_t* const physical_word = reinterpret_cast<uint64_t*>(
      TranslateUserModePhysical(memory_, physical_address));
  std::atomic_ref<uint64_t> atomic_physical_word(*physical_word);
  const bool exchanged = atomic_physical_word.compare_exchange_strong(
      expected_value, desired_value, std::memory_order_acq_rel,
      std::memory_order_acquire);
  *observed_value = expected_value;
  return exchanged;
}

void UserModePhysicalReadTransaction::CopyWritePrevalidated(
    uint32_t physical_address, std::span<const uint8_t> source) const {
  EnsureLocked();
  assert_false(source.empty());
  assert_not_null(source.data());
  std::memcpy(TranslateUserModePhysical(memory_, physical_address),
              source.data(), source.size());
}

}  // namespace xe::kernel

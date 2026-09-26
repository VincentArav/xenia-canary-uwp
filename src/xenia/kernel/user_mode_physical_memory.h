/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_PHYSICAL_MEMORY_H_
#define XENIA_KERNEL_USER_MODE_PHYSICAL_MEMORY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xenia/base/mutex.h"

namespace xe {
class Memory;
}  // namespace xe

namespace xe::kernel {

class UserModeGuestDataAccessor;

enum class UserModePhysicalReadDiagnostic {
  kNone,
  kInvalidSize,
  kAddressOverflow,
  kOutsidePhysicalMemory,
  kUnexpectedPhysicalHeapLayout,
  kPageQueryFailed,
  kPageNotReserved,
  kPageNotCommitted,
  kPageNotReadable,
};

struct UserModePhysicalReadResult {
  UserModePhysicalReadDiagnostic diagnostic =
      UserModePhysicalReadDiagnostic::kNone;
  uint32_t requested_physical_address = 0;
  size_t requested_size = 0;
  std::optional<uint32_t> failing_page_physical_address;

  explicit operator bool() const {
    return diagnostic == UserModePhysicalReadDiagnostic::kNone;
  }
};

enum class UserModePhysicalWriteProbeDiagnostic {
  kNone,
  kInvalidSize,
  kAddressOverflow,
  kOutsidePhysicalMemory,
  kUnexpectedPhysicalHeapLayout,
  kPageQueryFailed,
  kPageNotReserved,
  kPageNotCommitted,
  kPageNotWritable,
};

struct UserModePhysicalWriteProbeResult {
  UserModePhysicalWriteProbeDiagnostic diagnostic =
      UserModePhysicalWriteProbeDiagnostic::kNone;
  uint32_t requested_physical_address = 0;
  size_t requested_size = 0;
  std::optional<uint32_t> failing_page_physical_address;
  static constexpr bool write_performed = false;

  explicit operator bool() const {
    return diagnostic == UserModePhysicalWriteProbeDiagnostic::kNone;
  }
};

// Reads guest physical memory for user-mode page walks, code and data, and
// probes writes, holding the global lock for its lifetime so commitment and
// protection can't change meanwhile. The checked data accessor also stores
// through it.
class UserModePhysicalReadTransaction final {
 public:
  explicit UserModePhysicalReadTransaction(Memory& memory);

  UserModePhysicalReadTransaction(const UserModePhysicalReadTransaction&) =
      delete;
  UserModePhysicalReadTransaction& operator=(
      const UserModePhysicalReadTransaction&) = delete;

  // Copies the complete range only after every touched 4 KiB physical page is
  // confirmed reserved, committed and readable. Empty, overflowing and
  // non-29-bit ranges fail without modifying destination. A page this thread
  // confirmed since the last change to physical heap state is not queried
  // again, and a read of such pages alone takes no lock.
  UserModePhysicalReadResult TryRead(uint32_t physical_address,
                                     std::span<uint8_t> destination) const;

  // Validates the full physical target range without reading or writing it.
  // Backing must be readable and have either Write or WriteCombine protection.
  UserModePhysicalWriteProbeResult ProbeWrite(uint32_t physical_address,
                                              size_t size) const;

 private:
  friend class UserModeGuestDataAccessor;

  // The caller must have prevalidated the complete range using ProbeWrite and
  // must retain this transaction through the immediately following mutation.
  bool NotifyWriteInvalidationCallbacks(uint32_t physical_address,
                                        size_t size) const;
  bool CompareExchangeBe32Prevalidated(uint32_t physical_address,
                                       uint32_t expected_raw_value,
                                       uint32_t desired_raw_value,
                                       uint32_t* observed_raw_value) const;
  bool CompareExchangeRaw32Prevalidated(uint32_t physical_address,
                                        uint32_t expected_value,
                                        uint32_t desired_value,
                                        uint32_t* observed_value) const;
  bool CompareExchangeRaw64Prevalidated(uint32_t physical_address,
                                        uint64_t expected_value,
                                        uint64_t desired_value,
                                        uint64_t* observed_value) const;
  void CopyWritePrevalidated(uint32_t physical_address,
                             std::span<const uint8_t> source) const;

  // Takes the global lock for the rest of the transaction.
  void EnsureLocked() const;

  Memory& memory_;
  // Taken on first use: reads of pages this thread already validated need no
  // lock (see TryRead). Every other operation holds it.
  mutable xe::global_unique_lock_type global_lock_;
  bool physical_heap_is_valid_ = false;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_PHYSICAL_MEMORY_H_

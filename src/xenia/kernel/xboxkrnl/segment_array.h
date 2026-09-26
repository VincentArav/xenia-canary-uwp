/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_SEGMENT_ARRAY_H_
#define XENIA_KERNEL_XBOXKRNL_SEGMENT_ARRAY_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace segment_array {

// MmLockAndMapSegmentArray maps an array of 4 KiB pages.
constexpr uint32_t kPageSize = 0x1000;

// Whether each page follows the previous one, so the pages are already mapped
// contiguously at pages[0].
inline bool IsContiguous(std::span<const uint32_t> pages) {
  for (size_t i = 1; i < pages.size(); ++i) {
    if (pages[i] != pages[0] + static_cast<uint32_t>(i) * kPageSize) {
      return false;
    }
  }
  return !pages.empty();
}

// Copies to target the bytes of mapped that differ from snapshot, the contents
// the mapping started with. Bytes the mapping's user left alone keep what
// target holds now.
inline void WriteBackChangedBytes(const uint8_t* mapped,
                                  const uint8_t* snapshot, uint8_t* target,
                                  size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (mapped[i] != snapshot[i]) {
      target[i] = mapped[i];
    }
  }
}

}  // namespace segment_array
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_SEGMENT_ARRAY_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_ICACHE_SWEEP_H_
#define XENIA_KERNEL_ICACHE_SWEEP_H_

#include <algorithm>
#include <cstdint>
#include <vector>

#include "xenia/memory.h"

namespace xe::kernel {

// Half-open range of guest physical bytes.
struct IcacheSweepPhysicalRange {
  uint32_t begin;
  uint32_t end;
};

struct IcacheSweepClassification {
  // Word-aligned physical ranges covered through the physical views.
  std::vector<IcacheSweepPhysicalRange> physical_ranges;
  // Part of the sweep lies outside the physical views: DefaultDirect code,
  // which isn't invalidated live.
  bool non_physical = false;
};

// Splits a KeSweepIcacheRange argument into the guest physical ranges it
// names. Only the physical-view heaps (0xA0000000 and up) map a virtual
// address to physical memory; Memory::GetPhysicalAddress would return a
// meaningless offset for any other heap. The end is clamped at 4 GiB.
inline IcacheSweepClassification ClassifyIcacheSweep(const Memory& memory,
                                                     uint32_t address,
                                                     uint32_t size) {
  IcacheSweepClassification classification;
  uint64_t begin = address;
  const uint64_t end = std::min<uint64_t>(begin + size, uint64_t(1) << 32);
  while (begin < end) {
    const BaseHeap* heap = memory.LookupHeap(static_cast<uint32_t>(begin));
    if (!heap) {
      classification.non_physical = true;
      break;
    }
    const uint64_t heap_end = uint64_t(heap->heap_base()) + heap->heap_size();
    const uint64_t segment_end = std::min(end, heap_end);
    if (segment_end <= begin) {
      classification.non_physical = true;
      break;
    }
    if (heap->heap_type() == HeapType::kGuestPhysical) {
      const uint32_t physical_begin =
          memory.GetPhysicalAddress(static_cast<uint32_t>(begin)) & ~3u;
      const uint32_t physical_last =
          memory.GetPhysicalAddress(static_cast<uint32_t>(segment_end - 1));
      classification.physical_ranges.push_back(
          {physical_begin, (physical_last | 3u) + 1});
    } else {
      classification.non_physical = true;
    }
    begin = segment_end;
  }
  return classification;
}

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_ICACHE_SWEEP_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xex_patch_plan.h"

#include <algorithm>

namespace xe::cpu {
namespace {

constexpr uint64_t kAddressSpaceEnd = uint64_t(1) << 32;

// Strictly below the end, so every size and end address fits in 32 bits.
bool FitsAt(uint32_t base, uint64_t size) {
  return uint64_t(base) + size < kAddressSpaceEnd;
}

}  // namespace

ImagePatchPlan PlanImagePatch(uint32_t old_base, uint32_t old_page_count,
                              uint32_t old_page_size, uint32_t new_base,
                              uint32_t new_page_count, uint32_t new_page_size) {
  ImagePatchPlan plan;
  plan.old_base = old_base;
  plan.new_base = new_base;
  plan.old_page_size = old_page_size;
  plan.new_page_size = new_page_size;
  plan.move = new_base != old_base;
  if (!old_page_size || !new_page_size) {
    return plan;
  }

  const uint64_t old_size = uint64_t(old_page_count) * old_page_size;
  const uint64_t new_size = uint64_t(new_page_count) * new_page_size;
  if (!FitsAt(old_base, old_size) || !FitsAt(new_base, new_size)) {
    return plan;
  }
  plan.old_size = uint32_t(old_size);
  plan.new_size = uint32_t(new_size);

  if (!plan.move) {
    // One base means one heap, so both sizes are in the same pages.
    if (old_page_size != new_page_size) {
      return plan;
    }
    if (new_size > old_size) {
      plan.grow = true;
      plan.alloc_address = old_base + plan.old_size;
      plan.alloc_size = plan.new_size - plan.old_size;
    } else if (new_size < old_size) {
      plan.trim_address = old_base + plan.new_size;
      plan.trim_size = plan.old_size - plan.new_size;
    }
    plan.valid = true;
    return plan;
  }

  if (!new_size || new_base % new_page_size) {
    return plan;
  }

  // The delta is applied in place and may copy from anywhere in the original
  // image, so the working image at the new base also covers the original
  // bytes. Pages past the patched image are trimmed afterwards.
  const uint64_t working_size =
      (std::max(old_size, new_size) + new_page_size - 1) / new_page_size *
      new_page_size;
  if (!FitsAt(new_base, working_size)) {
    return plan;
  }
  plan.alloc_address = new_base;
  plan.alloc_size = uint32_t(working_size);
  plan.copy_size = plan.old_size;
  if (working_size > new_size) {
    plan.trim_address = new_base + plan.new_size;
    plan.trim_size = uint32_t(working_size - new_size);
  }
  plan.valid = true;
  return plan;
}

}  // namespace xe::cpu

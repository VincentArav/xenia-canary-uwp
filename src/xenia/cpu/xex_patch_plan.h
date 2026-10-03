/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_XEX_PATCH_PLAN_H_
#define XENIA_CPU_XEX_PATCH_PLAN_H_

#include <cstdint>

namespace xe::cpu {

// How a XEX image's memory must change when a delta title update (XEXP) is
// applied to it. The patched header may give the image a different size and,
// rarely, a different base address (the XNA player's title update moves the
// image from the 4 KB-page 0x9xxxxxxx heap to the 64 KB-page 0x8xxxxxxx heap).
//
// XEX page descriptor counts are in pages of the heap the image lives in, so
// each image size is computed with the page size of its own heap. Page sizes
// are passed in by the caller; this has no dependency on Memory.
struct ImagePatchPlan {
  // Inputs, as given to PlanImagePatch.
  uint32_t old_base = 0;
  uint32_t new_base = 0;
  uint32_t old_page_size = 0;
  uint32_t new_page_size = 0;

  // Image sizes in bytes (page descriptor count times heap page size).
  uint32_t old_size = 0;
  uint32_t new_size = 0;

  // False when the inputs can't describe a mappable image: a zero page size,
  // a size or range that overflows the 32-bit address space, or a moved image
  // that is empty or not aligned to its heap's pages. Nothing below is
  // meaningful then.
  bool valid = false;

  // The patched header gives the image a new base. The original image bytes
  // must be carried over to new_base before the delta is applied (it is
  // applied in place) and the old allocation released.
  bool move = false;
  // Same base, larger image: extra pages are needed after the old image.
  bool grow = false;

  // Range to reserve and commit before the delta is applied, at new_page_size
  // granularity (alloc_size == 0: none). For a move this is the whole working
  // image at new_base, big enough for both the original bytes the delta may
  // read and the patched image.
  uint32_t alloc_address = 0;
  uint32_t alloc_size = 0;
  // Leading bytes of the original image to carry over to new_base (move only).
  uint32_t copy_size = 0;

  // Range to decommit once the delta applied successfully because the patched
  // image is smaller than the memory it was built in (trim_size == 0: none).
  uint32_t trim_address = 0;
  uint32_t trim_size = 0;
};

// old_page_count / new_page_count are the summed page descriptor counts of the
// image before and after the header patch. old_page_size / new_page_size are
// the page sizes of the heaps containing old_base and new_base.
ImagePatchPlan PlanImagePatch(uint32_t old_base, uint32_t old_page_count,
                              uint32_t old_page_size, uint32_t new_base,
                              uint32_t new_page_count, uint32_t new_page_size);

}  // namespace xe::cpu

#endif  // XENIA_CPU_XEX_PATCH_PLAN_H_

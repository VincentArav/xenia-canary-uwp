/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_MEDIA_H_
#define XENIA_XEFU_XEFU_MEDIA_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/xefu/xbe_metadata.h"

namespace xe::vfs {
class Device;
}  // namespace xe::vfs

namespace xe::xefu {

// The game's executable at the root of an original Xbox disc.
constexpr std::string_view kXefuDiscXbeName = "default.xbe";
// Larger than any original Xbox executable; bounds the read into memory.
constexpr uint64_t kXefuDiscXbeMaxSize = 256ull * 1024 * 1024;

struct XefuDiscImageXbe {
  XbeMetadataError error = XbeMetadataError::kNone;
  // The executable's name as recorded on the disc.
  std::string name;
  std::vector<uint8_t> bytes;

  explicit operator bool() const { return error == XbeMetadataError::kNone; }
};

// Whether a disc image holds an original Xbox game: default.xbe at its root
// and no default.xex. Accepts the images xemu plays (XISO, the XDVDFS
// partition alone) and full XGD1 dumps.
bool IsXefuDiscImage(const std::filesystem::path& image_path);

// Reads default.xbe from a disc image, or through an initialized disc device.
XefuDiscImageXbe ReadXefuDiscImageXbe(const std::filesystem::path& image_path);
XefuDiscImageXbe ReadXefuDiscImageXbe(vfs::Device* device);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_MEDIA_H_

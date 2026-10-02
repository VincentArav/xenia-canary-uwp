/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_media.h"

#include <memory>

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

namespace xe::xefu {
namespace {

std::unique_ptr<vfs::Device> OpenDiscImage(
    const std::filesystem::path& image_path) {
  auto device = std::make_unique<vfs::DiscImageDevice>("", image_path);
  if (!device->Initialize()) {
    return nullptr;
  }
  return device;
}

}  // namespace

bool IsXefuDiscImage(const std::filesystem::path& image_path) {
  std::unique_ptr<vfs::Device> device = OpenDiscImage(image_path);
  if (!device) {
    return false;
  }
  vfs::Entry* xbe = device->ResolvePath(kXefuDiscXbeName);
  return xbe && !(xbe->attributes() & vfs::kFileAttributeDirectory) &&
         !device->ResolvePath("default.xex");
}

XefuDiscImageXbe ReadXefuDiscImageXbe(const std::filesystem::path& image_path) {
  std::unique_ptr<vfs::Device> device = OpenDiscImage(image_path);
  if (!device) {
    XefuDiscImageXbe result;
    result.error = std::filesystem::exists(image_path)
                       ? XbeMetadataError::kReadFailed
                       : XbeMetadataError::kFileNotFound;
    return result;
  }
  return ReadXefuDiscImageXbe(device.get());
}

XefuDiscImageXbe ReadXefuDiscImageXbe(vfs::Device* device) {
  XefuDiscImageXbe result;
  vfs::Entry* entry = device ? device->ResolvePath(kXefuDiscXbeName) : nullptr;
  if (!entry || (entry->attributes() & vfs::kFileAttributeDirectory)) {
    result.error = XbeMetadataError::kFileNotFound;
    return result;
  }
  if (entry->size() > kXefuDiscXbeMaxSize) {
    result.error = XbeMetadataError::kReadFailed;
    return result;
  }
  // A truncated image: the entry's data must lie inside the mapping.
  if (auto* disc_entry = dynamic_cast<vfs::DiscImageEntry*>(entry)) {
    const MappedMemory* mapping = disc_entry->mmap();
    if (!mapping || disc_entry->data_offset() > mapping->size() ||
        disc_entry->data_size() > mapping->size() - disc_entry->data_offset()) {
      result.error = XbeMetadataError::kReadFailed;
      return result;
    }
  }
  vfs::File* file = nullptr;
  if (entry->Open(filesystem::FileAccess::kFileReadData, &file) !=
          X_STATUS_SUCCESS ||
      !file) {
    result.error = XbeMetadataError::kReadFailed;
    return result;
  }
  result.bytes.resize(entry->size());
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(result.bytes, 0, &bytes_read);
  file->Destroy();
  if (status != X_STATUS_SUCCESS || bytes_read != result.bytes.size()) {
    result.bytes.clear();
    result.error = XbeMetadataError::kReadFailed;
    return result;
  }
  result.name = entry->name();
  return result;
}

}  // namespace xe::xefu

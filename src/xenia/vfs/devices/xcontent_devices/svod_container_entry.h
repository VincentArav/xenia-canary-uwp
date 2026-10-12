/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_
#define XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_

#include <cstring>
#include <map>

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/vfs/devices/xcontent_container_entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

using MultiMemoryMap = std::map<size_t, std::unique_ptr<MappedMemory>>;

#if XE_PLATFORM_WINRT
// Memory-mapping files can fail on UWP, so SVOD data fragments fall back to
// reads through a file handle. data() is null for these.
class SvodFileFragment final : public MappedMemory {
 public:
  SvodFileFragment(std::unique_ptr<filesystem::FileHandle> handle, size_t size)
      : MappedMemory(nullptr, size), handle_(std::move(handle)) {}

  bool Read(size_t offset, void* buffer, size_t length) const {
    size_t bytes_read = 0;
    return handle_->Read(offset, buffer, length, &bytes_read) &&
           bytes_read == length;
  }

 private:
  std::unique_ptr<filesystem::FileHandle> handle_;
};
#endif  // XE_PLATFORM_WINRT

// Copies length bytes at offset from a data fragment, mapped or not.
inline bool ReadSvodFragment(const MappedMemory& file, size_t offset,
                             void* buffer, size_t length) {
  if (offset > file.size() || file.size() - offset < length) {
    return false;
  }
  if (file.data()) {
    std::memcpy(buffer, file.data() + offset, length);
    return true;
  }
#if XE_PLATFORM_WINRT
  return static_cast<const SvodFileFragment&>(file).Read(offset, buffer,
                                                         length);
#else
  return false;
#endif  // XE_PLATFORM_WINRT
}

class XContentContainerDevice;

class SvodContainerEntry : public XContentContainerEntry {
 public:
  SvodContainerEntry(Device* device, Entry* parent, const std::string_view path,
                     MultiMemoryMap* files);
  ~SvodContainerEntry() override;

  static std::unique_ptr<SvodContainerEntry> Create(Device* device,
                                                    Entry* parent,
                                                    const std::string_view name,
                                                    MultiMemoryMap* files);

  MultiMemoryMap* files() const { return files_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

 private:
  bool DeleteEntryInternal(Entry* entry) override;

  MultiMemoryMap* files_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_XCONTENT_SVOD_CONTAINER_ENTRY_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_SCOPED_VFS_MOUNT_H_
#define XENIA_VFS_SCOPED_VFS_MOUNT_H_

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "xenia/vfs/virtual_file_system.h"

namespace xe::vfs {

// Owns VFS devices and symbolic links: one token for a mount group, or one per
// individually registered member. The VirtualFileSystem must outlive the
// scope.
class ScopedVfsMount {
 public:
  explicit ScopedVfsMount(VirtualFileSystem* file_system);
  ~ScopedVfsMount();

  ScopedVfsMount(const ScopedVfsMount&) = delete;
  ScopedVfsMount& operator=(const ScopedVfsMount&) = delete;
  ScopedVfsMount(ScopedVfsMount&& other) noexcept;
  ScopedVfsMount& operator=(ScopedVfsMount&& other) noexcept;

  // Initializes and registers a device; on failure the device is destroyed.
  bool MountDevice(std::unique_ptr<Device> device);
  bool RegisterSymbolicLink(std::string_view path, std::string_view target);
  // Registers initialized devices and links atomically. Can't be mixed with
  // the individual registration methods.
  bool RegisterMountGroup(VirtualFileSystem::MountGroup group);

  // Unpublishes an atomic group in one operation, or unregisters individually
  // owned symbolic links and devices in reverse order.
  void Reset();

  bool empty() const {
    return mount_group_token_ == VirtualFileSystem::kInvalidRegistrationToken &&
           device_tokens_.empty() && symbolic_link_tokens_.empty();
  }

 private:
  VirtualFileSystem* file_system_ = nullptr;
  uint64_t mount_group_token_ = VirtualFileSystem::kInvalidRegistrationToken;
  std::vector<uint64_t> device_tokens_;
  std::vector<uint64_t> symbolic_link_tokens_;
};

}  // namespace xe::vfs

#endif  // XENIA_VFS_SCOPED_VFS_MOUNT_H_

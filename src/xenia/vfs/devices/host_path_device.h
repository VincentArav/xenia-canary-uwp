/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_
#define XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_

#include <string>
#include <vector>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class HostPathEntry;

class HostPathDevice : public Device {
 public:
  // Limits of a safe read-only tree, including any writable root overlay.
  static constexpr size_t kSafeTreeMaximumDepth = 64;
  static constexpr size_t kSafeTreeMaximumEntries = 1000000;
  static constexpr size_t kSafeTreeMaximumRelativePathBytes = 4096;

  enum class TraversalPolicy {
    // Mounts the host tree as it is.
    kLegacy,
    // Rejects links/reparse points, special files and case-insensitive sibling
    // collisions while constructing a read-only entry tree.
    kSafeReadOnlyTree,
  };

  // writable_root_overlay optionally names a separate host directory whose
  // contents appear as writable root children of an allowlisted safe
  // read-only tree. New root children are created there; the allowlisted
  // read-only entries remain read-only and may not be shadowed.
  HostPathDevice(const std::string_view mount_path,
                 const std::filesystem::path& host_path, bool read_only,
                 std::vector<std::string> root_entry_allowlist = {},
                 TraversalPolicy traversal_policy = TraversalPolicy::kLegacy,
                 std::filesystem::path writable_root_overlay = {});
  ~HostPathDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return read_only_; }
  bool uses_safe_read_only_tree() const {
    return traversal_policy_ == TraversalPolicy::kSafeReadOnlyTree;
  }
  const std::filesystem::path& writable_root_overlay() const {
    return writable_root_overlay_;
  }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 protected:
  friend class HostPathEntry;
  std::filesystem::path host_path() const { return host_path_; }

 private:
  // Adds the children of host_directory to parent_entry. Overlay children are
  // writable and must not collide with any existing child.
  bool PopulateEntry(HostPathEntry* parent_entry,
                     const std::filesystem::path& host_directory,
                     bool writable_overlay, size_t depth);

  std::string name_;
  std::filesystem::path host_path_;
  std::filesystem::path writable_root_overlay_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<std::string> root_entry_allowlist_;
  TraversalPolicy traversal_policy_;
  size_t safe_tree_entry_count_ = 0;
  bool read_only_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_DEVICE_H_

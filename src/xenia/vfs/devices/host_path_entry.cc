/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_entry.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_file.h"

namespace xe {
namespace vfs {
namespace {

bool IsStillSafeTreeEntry(const HostPathDevice* device,
                          const std::filesystem::path& path,
                          bool expect_directory) {
  if (!device->uses_safe_read_only_tree()) {
    return true;
  }
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, status_error);
  return !status_error && !filesystem::IsSymlinkOrReparsePoint(path) &&
         (expect_directory ? std::filesystem::is_directory(status)
                           : std::filesystem::is_regular_file(status));
}

// A guest-supplied name must stay one component of its parent's host
// directory: no separators, dot components, or ':' (a Windows drive-relative
// prefix such as "G:name", or an alternate data stream).
bool IsSafeHostComponent(std::string_view name, bool safe_tree) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
      name.find_first_of("\\/:") != std::string_view::npos ||
      name.find('\0') != std::string_view::npos) {
    return false;
  }
  // Windows drops trailing dots and spaces, so in a safe tree "a." could alias
  // a sibling "a" on the host.
  return !safe_tree || (name.back() != '.' && name.back() != ' ');
}

}  // namespace

HostPathEntry::HostPathEntry(Device* device, Entry* parent,
                             const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() = default;

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     xe::filesystem::FileInfo file_info,
                                     bool writable_overlay) {
  auto path = xe::utf8::join_guest_paths(parent->path(),
                                         xe::path_to_utf8(file_info.name));
  auto entry = new HostPathEntry(device, parent, path, full_path);
  entry->writable_overlay_ = writable_overlay;

  entry->create_timestamp_ = file_info.create_timestamp;
  entry->access_timestamp_ = file_info.access_timestamp;
  entry->write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal;
    if (entry->is_read_only()) {
      entry->attributes_ |= kFileAttributeReadOnly;
    }
    entry->size_ = file_info.total_size;
    entry->allocation_size_ =
        xe::round_up(file_info.total_size, device->bytes_per_sector());
  }
  return entry;
}

bool HostPathEntry::is_read_only() const {
  return !writable_overlay_ && Entry::is_read_only();
}

bool HostPathEntry::can_create_children() const {
  return !create_host_path_.empty() || !is_read_only();
}

std::filesystem::path HostPathEntry::ChildHostPath(const Entry& child) const {
  const bool in_overlay = !create_host_path_.empty() && !child.is_read_only();
  return (in_overlay ? create_host_path_ : host_path_) /
         xe::to_path(child.name());
}

size_t HostPathEntry::depth() const {
  size_t depth = 0;
  for (const Entry* ancestor = parent(); ancestor;
       ancestor = ancestor->parent()) {
    ++depth;
  }
  return depth;
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  constexpr uint32_t kWriteAccessMask =
      FileAccess::kGenericWrite | FileAccess::kGenericAll |
      FileAccess::kFileWriteData | FileAccess::kFileAppendData;
  if (is_read_only() && (desired_access & kWriteAccessMask)) {
    XELOGE("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }
  const auto* host_device = static_cast<const HostPathDevice*>(device_);
  if (!IsStillSafeTreeEntry(host_device, host_path_,
                            (attributes_ & kFileAttributeDirectory) != 0)) {
    XELOGE("Safe host path entry changed before open");
    return X_STATUS_ACCESS_DENIED;
  }
  auto file_handle =
      xe::filesystem::FileHandle::OpenExisting(host_path_, desired_access);
  if (!file_handle) {
    // TODO(benvanik): pick correct response.
    return X_STATUS_NO_SUCH_FILE;
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

std::unique_ptr<MappedMemory> HostPathEntry::OpenMapped(MappedMemory::Mode mode,
                                                        size_t offset,
                                                        size_t length) {
  if (is_read_only() && mode != MappedMemory::Mode::kRead) {
    XELOGE("Attempting to map a file for write access on read-only device");
    return nullptr;
  }
  const auto* host_device = static_cast<const HostPathDevice*>(device_);
  if (!IsStillSafeTreeEntry(host_device, host_path_, false)) {
    XELOGE("Safe host path entry changed before mapped open");
    return nullptr;
  }
  return MappedMemory::Open(host_path_, mode, offset, length);
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(
    const std::string_view name, uint32_t attributes) {
  const bool safe_tree =
      static_cast<const HostPathDevice*>(device_)->uses_safe_read_only_tree();
  if (!IsSafeHostComponent(name, safe_tree)) {
    XELOGE("Refusing to create unsafe host path entry name '{}'", name);
    return nullptr;
  }
  // Stay within the limits the next mount enforces, so the guest cannot leave
  // an overlay that fails to populate.
  if (safe_tree && (((attributes & kFileAttributeDirectory) &&
                     depth() + 1 > HostPathDevice::kSafeTreeMaximumDepth) ||
                    path().size() + 1 + name.size() >
                        HostPathDevice::kSafeTreeMaximumRelativePathBytes)) {
    XELOGE("Refusing to create host path entry beyond safe tree limits: {}",
           name);
    return nullptr;
  }
  auto full_path =
      (create_host_path_.empty() ? host_path_ : create_host_path_) /
      xe::to_path(name);
  if (attributes & kFileAttributeDirectory) {
    if (!std::filesystem::create_directories(full_path)) {
      return nullptr;
    }
  } else {
    auto file = xe::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
  auto file_info = xe::filesystem::GetInfo(full_path);
  if (!file_info) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(
      HostPathEntry::Create(device_, this, full_path, file_info.value(),
                            writable_overlay_ || !create_host_path_.empty()));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  const std::filesystem::path full_path = ChildHostPath(*entry);
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Skip directories, they we're handled above.
    if (std::filesystem::is_directory(full_path)) {
      return false;
    }

    if (std::filesystem::exists(full_path)) {
      const auto result = std::filesystem::remove(full_path, ec);
      if (ec) {
        XELOGE("{}: Cannot remove file entry. File: {} Error: {}", __func__,
               full_path, ec.message());
        return false;
      }
    }
    return true;
  }
}

bool HostPathEntry::RenameEntryInternal(
    const std::vector<std::string_view>& path_parts) {
  // The root is never renamed, and neither are read-only entries.
  if (!parent_ || is_read_only()) {
    XELOGE("RenameEntryInternal: refusing to rename read-only entry '{}'",
           xe::path_to_utf8(host_path_));
    return false;
  }
  auto* host_device = static_cast<HostPathDevice*>(device_);
  const bool safe_tree = host_device->uses_safe_read_only_tree();
  // Every component must stay inside the host root: no "..", drive prefixes
  // or separators.
  if (path_parts.empty() ||
      (safe_tree &&
       path_parts.size() > HostPathDevice::kSafeTreeMaximumDepth)) {
    return false;
  }
  for (std::string_view part : path_parts) {
    if (!IsSafeHostComponent(part, safe_tree)) {
      XELOGE("RenameEntryInternal: refusing unsafe target component '{}'",
             part);
      return false;
    }
  }
  if (safe_tree) {
    // Never give an existing entry, such as an allowlisted read-only one, a
    // second host file; the next mount would also reject the collision.
    const std::string guest_path = xe::utf8::join_guest_paths(path_parts);
    const Entry* existing = host_device->ResolvePath(guest_path);
    if ((existing && existing != this) ||
        guest_path.size() > HostPathDevice::kSafeTreeMaximumRelativePathBytes) {
      XELOGE("RenameEntryInternal: refusing colliding target '{}'", guest_path);
      return false;
    }
  }
  // Overlay entries may only move within the overlay, never into the
  // read-only tree that shares their guest namespace.
  std::filesystem::path new_host_path =
      writable_overlay_ && !host_device->writable_root_overlay().empty()
          ? host_device->writable_root_overlay()
          : host_device->host_path();
  for (std::string_view part : path_parts) {
    new_host_path /= xe::to_path(part);
  }

  std::error_code ec;
  std::filesystem::rename(host_path_, new_host_path, ec);
  if (ec) {
    XELOGE("RenameEntryInternal: Failed to rename '{}' to '{}': {}",
           xe::path_to_utf8(host_path_), xe::path_to_utf8(new_host_path),
           ec.message());
    return false;
  }
  host_path_ = std::move(new_host_path);
  return true;
}

void HostPathEntry::update() {
  auto file_info = xe::filesystem::GetInfo(host_path_);
  if (!file_info) {
    return;
  }
  if (file_info->type == xe::filesystem::FileInfo::Type::kFile) {
    size_ = file_info->total_size;
    allocation_size_ =
        xe::round_up(file_info->total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetAttributes(uint64_t attributes) {
  if (is_read_only()) {
    return false;
  }
  return xe::filesystem::SetAttributes(host_path_, attributes);
}

bool HostPathEntry::SetCreateTimestamp(uint64_t timestamp) {
  if (is_read_only()) {
    XELOGW(
        "{} - Tried to change read-only creation timestamp for file: {} to: {}",
        __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change creation timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

bool HostPathEntry::SetAccessTimestamp(uint64_t timestamp) {
  if (is_read_only()) {
    XELOGW(
        "{} - Tried to change read-only access timestamp for file: {} to: {}",
        __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change access timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

bool HostPathEntry::SetWriteTimestamp(uint64_t timestamp) {
  if (is_read_only()) {
    XELOGW("{} - Tried to change read-only write timestamp for file: {} to: {}",
           __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change write timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

}  // namespace vfs
}  // namespace xe

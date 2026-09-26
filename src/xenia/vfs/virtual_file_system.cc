/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/virtual_file_system.h"

#include <algorithm>
#include <new>

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

#include "devices/host_path_entry.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/xfile.h"

namespace xe {
namespace vfs {

using namespace xe::literals;

namespace {

constexpr size_t kMaxSymbolicLinkResolutionHops = 64;

bool IsGuestPathParentOrEqual(std::string_view parent, std::string_view child) {
  if (parent.empty() || !xe::utf8::starts_with_case(child, parent)) {
    return false;
  }
  if (parent.size() == child.size()) {
    return true;
  }
  // canonicalize_guest_path removes trailing separators except for an
  // absolute root. Drive aliases still require the separator: "X:foo" is not
  // a child of "X:", while "X:\\foo" is.
  return parent.back() == '\\' || child[parent.size()] == '\\';
}

bool GuestPathNamespacesOverlap(std::string_view left, std::string_view right) {
  return IsGuestPathParentOrEqual(left, right) ||
         IsGuestPathParentOrEqual(right, left);
}

}  // namespace

VirtualFileSystem::VirtualFileSystem() {}

VirtualFileSystem::~VirtualFileSystem() {
  // Delete all devices.
  // This will explode if anyone is still using data from them.
  Clear();
}

void VirtualFileSystem::Clear() {
  auto global_lock = global_critical_region_.Acquire();
  symlinks_.clear();
  devices_.clear();
  mount_groups_.clear();
}

VirtualFileSystem::RegistrationToken
VirtualFileSystem::AllocateRegistrationTokenLocked() {
  RegistrationToken token = next_registration_token_++;
  if (token == kInvalidRegistrationToken) {
    token = next_registration_token_++;
  }
  return token;
}

bool VirtualFileSystem::HasMountGroupPathConflictLocked(
    std::string_view normalized_path) const {
  return std::find_if(mount_groups_.cbegin(), mount_groups_.cend(),
                      [&](const RegisteredMountGroup& group) {
                        return std::find_if(
                                   group.namespace_paths.cbegin(),
                                   group.namespace_paths.cend(),
                                   [&](const std::string& reserved_path) {
                                     return GuestPathNamespacesOverlap(
                                         normalized_path, reserved_path);
                                   }) != group.namespace_paths.cend();
                      }) != mount_groups_.cend();
}

bool VirtualFileSystem::RegisterDevice(std::unique_ptr<Device> device,
                                       RegistrationToken* registration_token) {
  if (!device) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_mount_path =
      xe::utf8::canonicalize_guest_path(device->mount_path());
  if (normalized_mount_path.empty()) {
    return false;
  }
  if (HasMountGroupPathConflictLocked(normalized_mount_path)) {
    XELOGE("Device path overlaps an active mount-group reservation: {}",
           device->mount_path());
    return false;
  }
  const auto existing =
      std::find_if(devices_.cbegin(), devices_.cend(), [&](const auto& item) {
        return xe::utf8::equal_case(
            xe::utf8::canonicalize_guest_path(item.device->mount_path()),
            normalized_mount_path);
      });
  if (existing != devices_.cend()) {
    XELOGE("Trying to re-register already registered device: {}",
           device->mount_path());
    return false;
  }

  XELOGD("Registered device: {}", device->mount_path());
  const RegistrationToken token = AllocateRegistrationTokenLocked();
  devices_.push_back({token, kInvalidRegistrationToken, std::move(device)});
  if (registration_token) {
    *registration_token = token;
  }
  return true;
}

bool VirtualFileSystem::UnregisterDevice(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  for (auto it = devices_.begin(); it != devices_.end(); ++it) {
    if (xe::utf8::equal_case(
            xe::utf8::canonicalize_guest_path(it->device->mount_path()),
            normalized_path)) {
      if (it->mount_group_token != kInvalidRegistrationToken) {
        return false;
      }
      XELOGD("Unregistered device: {}", it->device->mount_path());
      devices_.erase(it);
      return true;
    }
  }
  return false;
}

bool VirtualFileSystem::UnregisterDevice(RegistrationToken registration_token) {
  if (registration_token == kInvalidRegistrationToken) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  const auto it =
      std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
        return device.mount_group_token == kInvalidRegistrationToken &&
               device.token == registration_token;
      });
  if (it == devices_.end()) {
    return false;
  }
  XELOGD("Unregistered device: {}", it->device->mount_path());
  devices_.erase(it);
  return true;
}

bool VirtualFileSystem::IsDeviceRegistered(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  return std::find_if(
             devices_.cbegin(), devices_.cend(), [&](const auto& device) {
               return xe::utf8::equal_case(xe::utf8::canonicalize_guest_path(
                                               device.device->mount_path()),
                                           normalized_path);
             }) != devices_.cend();
}

bool VirtualFileSystem::HasDeviceMountPathConflict(
    const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  return std::find_if(devices_.cbegin(), devices_.cend(),
                      [&](const auto& device) {
                        const std::string normalized_mount_path =
                            xe::utf8::canonicalize_guest_path(
                                device.device->mount_path());
                        return GuestPathNamespacesOverlap(
                            normalized_path, normalized_mount_path);
                      }) != devices_.cend();
}

bool VirtualFileSystem::RegisterSymbolicLink(
    const std::string_view path, const std::string_view target,
    RegistrationToken* registration_token) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  if (normalized_path.empty()) {
    return false;
  }
  if (HasMountGroupPathConflictLocked(normalized_path)) {
    XELOGE("Symbolic-link path overlaps an active mount-group reservation: {}",
           path);
    return false;
  }
  auto it =
      std::find_if(symlinks_.cbegin(), symlinks_.cend(), [&](const auto& s) {
        return xe::utf8::equal_case(normalized_path, s.first);
      });
  if (it != symlinks_.end()) {
    XELOGE("Trying to re-register already registered symbolic link: {} => {}",
           path, target);
    return false;
  }

  const RegistrationToken token = AllocateRegistrationTokenLocked();
  symlinks_.insert({normalized_path,
                    {token, kInvalidRegistrationToken, std::string(target)}});
  if (registration_token) {
    *registration_token = token;
  }
  XELOGD("Registered symbolic link: {} => {}", path, target);

  return true;
}

bool VirtualFileSystem::UnregisterSymbolicLink(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  auto it =
      std::find_if(symlinks_.cbegin(), symlinks_.cend(), [&](const auto& s) {
        return xe::utf8::equal_case(normalized_path, s.first);
      });
  if (it == symlinks_.end()) {
    return false;
  }
  if (it->second.mount_group_token != kInvalidRegistrationToken) {
    return false;
  }
  XELOGD("Unregistered symbolic link: {} => {}", it->first, it->second.target);

  symlinks_.erase(it);
  return true;
}

bool VirtualFileSystem::UnregisterSymbolicLink(
    RegistrationToken registration_token) {
  if (registration_token == kInvalidRegistrationToken) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  const auto it = std::find_if(
      symlinks_.begin(), symlinks_.end(), [&](const auto& symbolic_link) {
        return symbolic_link.second.mount_group_token ==
                   kInvalidRegistrationToken &&
               symbolic_link.second.token == registration_token;
      });
  if (it == symlinks_.end()) {
    return false;
  }
  XELOGD("Unregistered symbolic link: {} => {}", it->first, it->second.target);
  symlinks_.erase(it);
  return true;
}

bool VirtualFileSystem::IsSymbolicLinkRegistered(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  auto it =
      std::find_if(symlinks_.cbegin(), symlinks_.cend(), [&](const auto& s) {
        return xe::utf8::equal_case(normalized_path, s.first);
      });

  return it != symlinks_.cend();
}

bool VirtualFileSystem::FindSymbolicLink(const std::string_view path,
                                         std::string& target) {
  auto global_lock = global_critical_region_.Acquire();
  const std::string normalized_path = xe::utf8::canonicalize_guest_path(path);
  auto best = symlinks_.cend();
  size_t best_length = 0;
  for (auto it = symlinks_.cbegin(); it != symlinks_.cend(); ++it) {
    if (IsGuestPathParentOrEqual(it->first, normalized_path) &&
        it->first.size() >= best_length) {
      best = it;
      best_length = it->first.size();
    }
  }
  if (best == symlinks_.cend()) {
    return false;
  }
  target = best->second.target;
  return true;
}

bool VirtualFileSystem::RegisterMountGroup(
    MountGroup group, RegistrationToken* registration_token) {
  if (!registration_token ||
      (group.devices.empty() && group.symbolic_links.empty())) {
    return false;
  }
  *registration_token = kInvalidRegistrationToken;

  std::vector<std::string> device_paths;
  std::vector<std::string> symbolic_link_paths;
  std::vector<std::string> namespace_paths;
  device_paths.reserve(group.devices.size());
  symbolic_link_paths.reserve(group.symbolic_links.size());
  namespace_paths.reserve(group.devices.size() + group.symbolic_links.size());

  for (const std::unique_ptr<Device>& device : group.devices) {
    if (!device) {
      return false;
    }
    std::string normalized_path =
        xe::utf8::canonicalize_guest_path(device->mount_path());
    if (normalized_path.empty() ||
        std::find_if(device_paths.cbegin(), device_paths.cend(),
                     [&](const std::string& existing_path) {
                       return xe::utf8::equal_case(normalized_path,
                                                   existing_path);
                     }) != device_paths.cend()) {
      return false;
    }
    device_paths.push_back(normalized_path);
    namespace_paths.push_back(std::move(normalized_path));
  }

  for (MountGroupSymbolicLink& symbolic_link : group.symbolic_links) {
    std::string normalized_path =
        xe::utf8::canonicalize_guest_path(symbolic_link.path);
    if (normalized_path.empty() || symbolic_link.target.empty() ||
        std::find_if(symbolic_link_paths.cbegin(), symbolic_link_paths.cend(),
                     [&](const std::string& existing_path) {
                       return GuestPathNamespacesOverlap(normalized_path,
                                                         existing_path);
                     }) != symbolic_link_paths.cend()) {
      return false;
    }
    // A link may not replace or contain a device of its own group; a link
    // inside one of its devices is allowed.
    if (std::find_if(
            device_paths.cbegin(), device_paths.cend(),
            [&](const std::string& device_path) {
              return xe::utf8::equal_case(normalized_path, device_path) ||
                     (GuestPathNamespacesOverlap(normalized_path,
                                                 device_path) &&
                      !IsGuestPathParentOrEqual(device_path, normalized_path));
            }) != device_paths.cend()) {
      return false;
    }
    symbolic_link_paths.push_back(normalized_path);
    symbolic_link.path = normalized_path;
    namespace_paths.push_back(std::move(normalized_path));
  }

  // Devices may nest within a group. Put the most specific first: resolution
  // takes the first matching device.
  std::stable_sort(
      group.devices.begin(), group.devices.end(),
      [](const std::unique_ptr<Device>& left,
         const std::unique_ptr<Device>& right) {
        return xe::utf8::canonicalize_guest_path(left->mount_path()).size() >
               xe::utf8::canonicalize_guest_path(right->mount_path()).size();
      });

  auto global_lock = global_critical_region_.Acquire();
  for (const std::string& proposed_path : namespace_paths) {
    const bool device_conflict =
        std::find_if(devices_.cbegin(), devices_.cend(),
                     [&](const RegisteredDevice& existing_device) {
                       return GuestPathNamespacesOverlap(
                           proposed_path,
                           xe::utf8::canonicalize_guest_path(
                               existing_device.device->mount_path()));
                     }) != devices_.cend();
    const bool symbolic_link_conflict =
        std::find_if(symlinks_.cbegin(), symlinks_.cend(),
                     [&](const auto& existing_symbolic_link) {
                       return GuestPathNamespacesOverlap(
                           proposed_path, existing_symbolic_link.first);
                     }) != symlinks_.cend();
    if (device_conflict || symbolic_link_conflict ||
        HasMountGroupPathConflictLocked(proposed_path)) {
      return false;
    }
  }

  const RegistrationToken group_token = AllocateRegistrationTokenLocked();
  auto rollback_group = [&]() {
    for (auto it = symlinks_.begin(); it != symlinks_.end();) {
      if (it->second.mount_group_token == group_token) {
        it = symlinks_.erase(it);
      } else {
        ++it;
      }
    }
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                  [&](const RegisteredDevice& device) {
                                    return device.mount_group_token ==
                                           group_token;
                                  }),
                   devices_.end());
    mount_groups_.erase(
        std::remove_if(mount_groups_.begin(), mount_groups_.end(),
                       [&](const RegisteredMountGroup& mount_group) {
                         return mount_group.token == group_token;
                       }),
        mount_groups_.end());
  };

  try {
    devices_.reserve(devices_.size() + group.devices.size());
    symlinks_.reserve(symlinks_.size() + group.symbolic_links.size());
    mount_groups_.reserve(mount_groups_.size() + 1);
    mount_groups_.push_back({group_token, std::move(namespace_paths)});
    for (std::unique_ptr<Device>& device : group.devices) {
      devices_.push_back(
          {kInvalidRegistrationToken, group_token, std::move(device)});
    }
    for (MountGroupSymbolicLink& symbolic_link : group.symbolic_links) {
      const bool inserted =
          symlinks_
              .emplace(
                  std::move(symbolic_link.path),
                  RegisteredSymbolicLink{kInvalidRegistrationToken, group_token,
                                         std::move(symbolic_link.target)})
              .second;
      if (!inserted) {
        rollback_group();
        return false;
      }
    }
  } catch (const std::bad_alloc&) {
    rollback_group();
    return false;
  }

  *registration_token = group_token;
  XELOGD("Registered VFS mount group: {}", group_token);
  return true;
}

bool VirtualFileSystem::UnregisterMountGroup(
    RegistrationToken registration_token) {
  if (registration_token == kInvalidRegistrationToken) {
    return false;
  }
  auto global_lock = global_critical_region_.Acquire();
  const auto group_it = std::find_if(mount_groups_.begin(), mount_groups_.end(),
                                     [&](const RegisteredMountGroup& group) {
                                       return group.token == registration_token;
                                     });
  if (group_it == mount_groups_.end()) {
    return false;
  }

  for (auto it = symlinks_.begin(); it != symlinks_.end();) {
    if (it->second.mount_group_token == registration_token) {
      it = symlinks_.erase(it);
    } else {
      ++it;
    }
  }
  devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                [&](const RegisteredDevice& device) {
                                  return device.mount_group_token ==
                                         registration_token;
                                }),
                 devices_.end());
  mount_groups_.erase(group_it);
  XELOGD("Unregistered VFS mount group: {}", registration_token);
  return true;
}

VirtualFileSystem::SymbolicLinkResolution
VirtualFileSystem::ResolveSymbolicLink(const std::string_view path,
                                       std::string& result) {
  result = xe::utf8::canonicalize_guest_path(path);
  size_t hop_count = 0;
  while (true) {
    auto best = symlinks_.cend();
    size_t best_length = 0;
    for (auto it = symlinks_.cbegin(); it != symlinks_.cend(); ++it) {
      if (IsGuestPathParentOrEqual(it->first, result) &&
          it->first.size() >= best_length) {
        best = it;
        best_length = it->first.size();
      }
    }
    if (best == symlinks_.cend()) {
      break;
    }
    // An acyclic chain can't cross more links than exist; the fixed ceiling
    // also bounds the work in a large namespace.
    if (hop_count++ >=
        std::min(symlinks_.size(), kMaxSymbolicLinkResolutionHops)) {
      result.clear();
      return SymbolicLinkResolution::kFailed;
    }
    // Found symlink!
    auto target_path = best->second.target;
    auto relative_path = result.substr(best->first.size());
    result = xe::utf8::canonicalize_guest_path(target_path + relative_path);
  }
  return hop_count ? SymbolicLinkResolution::kResolved
                   : SymbolicLinkResolution::kUnchanged;
}

Entry* VirtualFileSystem::ResolvePath(const std::string_view path) {
  auto global_lock = global_critical_region_.Acquire();

  // Resolve relative paths
  auto normalized_path(xe::utf8::canonicalize_guest_path(path));

  // Resolve symlinks.
  std::string resolved_path;
  switch (ResolveSymbolicLink(normalized_path, resolved_path)) {
    case SymbolicLinkResolution::kResolved:
      normalized_path = resolved_path;
      break;
    case SymbolicLinkResolution::kFailed:
      XELOGE("ResolvePath({}) failed - symbolic-link cycle", path);
      return nullptr;
    case SymbolicLinkResolution::kUnchanged:
      break;
  }

  // Find the device.
  auto it =
      std::find_if(devices_.cbegin(), devices_.cend(), [&](const auto& d) {
        return IsGuestPathParentOrEqual(
            xe::utf8::canonicalize_guest_path(d.device->mount_path()),
            normalized_path);
      });
  if (it == devices_.cend()) {
    // Supress logging the error for ShaderDumpxe:\CompareBackEnds as this is
    // not an actual problem nor something we care about.
    if (path != "ShaderDumpxe:\\CompareBackEnds") {
      XELOGE("ResolvePath({}) failed - device not found", path);
    }
    return nullptr;
  }

  const auto& device = it->device;
  const std::string normalized_mount_path =
      xe::utf8::canonicalize_guest_path(device->mount_path());
  auto relative_path = normalized_path.substr(normalized_mount_path.size());
  return device->ResolvePath(relative_path);
}

Entry* VirtualFileSystem::CreatePath(const std::string_view path,
                                     uint32_t attributes) {
  // Create all required directories recursively.
  auto path_parts = xe::utf8::split_path(path);
  if (path_parts.size() < 2) {
    return nullptr;
  }
  // Start from the deepest existing ancestor. An absolute device path keeps
  // its leading separator, and its device root may span several components
  // ("\Device\Harddisk0\Partition1"), so the first component alone need not
  // resolve.
  const bool absolute = path.front() == '\\' || path.front() == '/';
  auto make_prefix = [&](size_t component_count) {
    std::string prefix = absolute ? "\\" : "";
    for (size_t i = 0; i < component_count; ++i) {
      if (i) {
        prefix += '\\';
      }
      prefix += path_parts[i];
    }
    return prefix;
  };
  Entry* parent_entry = nullptr;
  size_t existing_count = path_parts.size() - 1;
  for (; existing_count; --existing_count) {
    parent_entry = ResolvePath(make_prefix(existing_count));
    if (parent_entry) {
      break;
    }
  }
  if (!parent_entry) {
    return nullptr;
  }
  for (size_t i = existing_count; i < path_parts.size() - 1; ++i) {
    auto child_entry =
        parent_entry->CreateEntry(path_parts[i], kFileAttributeDirectory);
    if (!child_entry) {
      return nullptr;
    }
    parent_entry = child_entry;
  }
  return parent_entry->CreateEntry(path_parts[path_parts.size() - 1],
                                   attributes);
}

bool VirtualFileSystem::DeletePath(const std::string_view path) {
  auto entry = ResolvePath(path);
  if (!entry) {
    return false;
  }
  auto parent = entry->parent();
  if (!parent) {
    // Can't delete root.
    return false;
  }
  return parent->Delete(entry);
}

X_STATUS VirtualFileSystem::OpenFile(Entry* root_entry,
                                     const std::string_view path,
                                     FileDisposition creation_disposition,
                                     uint32_t desired_access, bool is_directory,
                                     bool is_non_directory, File** out_file,
                                     FileAction* out_action) {
  // TODO(gibbed): should 'is_directory' remain as a bool or should it be
  // flipped to a generic FileAttributeFlags?

  // Cleanup access.
  if (desired_access & FileAccess::kGenericRead) {
    desired_access |= FileAccess::kFileReadData;
  }
  if (desired_access & FileAccess::kGenericWrite) {
    desired_access |= FileAccess::kFileWriteData;
  }
  if (desired_access & FileAccess::kGenericAll) {
    desired_access |= FileAccess::kFileReadData | FileAccess::kFileWriteData;
  }

  // Lookup host device/parent path.
  // If no device or parent, fail.
  Entry* parent_entry = nullptr;
  Entry* entry = nullptr;

  auto base_path = xe::utf8::find_base_guest_path(path);
  if (!base_path.empty()) {
    parent_entry = !root_entry ? ResolvePath(base_path)
                               : root_entry->ResolvePath(base_path);
    if (!parent_entry) {
      *out_action = FileAction::kDoesNotExist;
      return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }

    auto file_name = xe::utf8::find_name_from_guest_path(path);
    entry = parent_entry->GetChild(file_name);
  } else {
    entry = !root_entry ? ResolvePath(path) : root_entry->GetChild(path);
  }

  if (entry) {
    if (entry->attributes() & kFileAttributeDirectory && is_non_directory) {
      return X_STATUS_FILE_IS_A_DIRECTORY;
    }

    // If the entry does not exist on the host then remove the cached entry.
    // The parent decides where a child lives: a root overlay stores writable
    // children outside the parent's listed host directory.
    if (parent_entry) {
      const xe::vfs::HostPathEntry* host_path =
          dynamic_cast<const xe::vfs::HostPathEntry*>(parent_entry);

      if (host_path) {
        auto const file_path = host_path->ChildHostPath(*entry);

        if (!std::filesystem::exists(file_path)) {
          // Remove cached entry
          entry->Delete();
          entry = nullptr;
        }
      }
    }
  }

  // Check if exists (if we need it to), or that it doesn't (if it shouldn't).
  switch (creation_disposition) {
    case FileDisposition::kOpen:
    case FileDisposition::kOverwrite:
      // Must exist.
      if (!entry) {
        *out_action = FileAction::kDoesNotExist;
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
      }
      break;
    case FileDisposition::kCreate:
      // Must not exist.
      if (entry) {
        *out_action = FileAction::kExists;
        return X_STATUS_OBJECT_NAME_COLLISION;
      }
      break;
    default:
      // Either way, ok.
      break;
  }

  // Verify permissions.
  bool wants_write = desired_access & FileAccess::kFileWriteData ||
                     desired_access & FileAccess::kFileAppendData;
  if (wants_write && ((parent_entry && !parent_entry->can_create_children()) ||
                      (entry && entry->is_read_only()))) {
    // Fail if read only device and wants write.
    // return X_STATUS_ACCESS_DENIED;
    // TODO(benvanik): figure out why games are opening read-only files with
    // write modes.
    assert_always();
    XELOGW("Attempted to open the file/dir for create/write");
    desired_access = FileAccess::kGenericRead | FileAccess::kFileReadData;
  }

  bool created = false;
  if (!entry) {
    // Remember that we are creating this new, instead of replacing.
    created = true;
    *out_action = FileAction::kCreated;
  } else {
    // May need to delete, if it exists.
    switch (creation_disposition) {
      case FileDisposition::kCreate:
        // Shouldn't be possible to hit this.
        assert_always();
        return X_STATUS_ACCESS_DENIED;
      case FileDisposition::kSuperscede:
        // Replace (by delete + recreate).
        if (!entry->Delete()) {
          return X_STATUS_ACCESS_DENIED;
        }
        entry = nullptr;
        *out_action = FileAction::kSuperseded;
        break;
      case FileDisposition::kOpen:
      case FileDisposition::kOpenIf:
        // Normal open.
        *out_action = FileAction::kOpened;
        break;
      case FileDisposition::kOverwrite:
      case FileDisposition::kOverwriteIf:
        // Overwrite (we do by delete + recreate).
        if (!entry->Delete()) {
          return X_STATUS_ACCESS_DENIED;
        }
        entry = nullptr;
        *out_action = FileAction::kOverwritten;
        break;
    }
  }
  if (!entry) {
    // Create if needed (either new or as a replacement).
    entry = CreatePath(
        path, !is_directory ? kFileAttributeNormal : kFileAttributeDirectory);
    if (!entry) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  // Open.
  auto result = entry->Open(desired_access, out_file);
  if (XFAILED(result)) {
    *out_action = FileAction::kDoesNotExist;
  }
  return result;
}

X_STATUS VirtualFileSystem::ExtractContentFile(Entry* entry,
                                               std::filesystem::path base_path,
                                               uint64_t& progress,
                                               bool extract_to_root) {
  // Allocate a buffer when needed.
  size_t buffer_size = 0;
  uint8_t* buffer = nullptr;

  XELOGI("Extracting file: {}", entry->path());

  auto dest_name =
      base_path / xe::to_path(utf8::fix_path_separators(entry->path()));

  if (extract_to_root) {
    dest_name = base_path / xe::to_path(entry->name());
  }

  if (entry->attributes() & kFileAttributeDirectory) {
    std::error_code error_code;
    std::filesystem::create_directories(dest_name, error_code);
    if (error_code) {
      return error_code.value();
    }
    return 0;
  }

  vfs::File* in_file = nullptr;
  X_STATUS result = entry->Open(FileAccess::kFileReadData, &in_file);
  if (result != X_STATUS_SUCCESS) {
    return result;
  }

  auto file = xe::filesystem::OpenFile(dest_name, "wb");
  if (!file) {
    in_file->Destroy();
    return 1;
  }
  constexpr size_t write_buffer_size = 4_MiB;

  if (entry->can_map()) {
    auto map = entry->OpenMapped(xe::MappedMemory::Mode::kRead);

    size_t remaining_size = map->size();
    size_t offset = 0;

    while (remaining_size > 0) {
      const auto bytes_to_read = std::min(write_buffer_size, remaining_size);
      fwrite(map->data() + offset, bytes_to_read, 1, file);
      offset += bytes_to_read;
      remaining_size -= bytes_to_read;
      progress += bytes_to_read;
    }
    map->Close();
  } else {
    size_t remaining_size = entry->size();
    size_t offset = 0;
    buffer = new uint8_t[write_buffer_size];

    while (remaining_size > 0) {
      size_t bytes_read = 0;
      in_file->ReadSync(std::span<uint8_t>(buffer, write_buffer_size), offset,
                        &bytes_read);
      fwrite(buffer, bytes_read, 1, file);
      offset += bytes_read;
      remaining_size -= bytes_read;
      progress += bytes_read;
    }
  }

  fclose(file);
  in_file->Destroy();

  if (buffer) {
    delete[] buffer;
  }
  return 0;
}

X_STATUS VirtualFileSystem::ExtractContentFiles(Device* device,
                                                std::filesystem::path base_path,
                                                uint64_t& progress) {
  // Run through all the files, breadth-first style.
  std::queue<vfs::Entry*> queue;
  auto root = device->ResolvePath("/");
  queue.push(root);

  while (!queue.empty()) {
    auto entry = queue.front();
    queue.pop();
    for (auto& entry : entry->children()) {
      queue.push(entry.get());
    }

    ExtractContentFile(entry, base_path, progress);
  }
  return X_STATUS_SUCCESS;
}

void VirtualFileSystem::ExtractContentHeader(Device* device,
                                             std::filesystem::path base_path) {
  const XContentContainerDevice* xcontent_device =
      ((XContentContainerDevice*)device);

  if (!std::filesystem::exists(base_path.parent_path())) {
    if (!std::filesystem::create_directories(base_path.parent_path())) {
      return;
    }
  }
  auto header_filename = base_path.filename().string() + ".header";
  auto header_path = base_path.parent_path() / header_filename;
  xe::filesystem::CreateEmptyFile(header_path);

  if (std::filesystem::exists(header_path)) {
    auto file = xe::filesystem::OpenFile(header_path, "wb");
    kernel::xam::XCONTENT_AGGREGATE_DATA data =
        xcontent_device->content_header();
    uint32_t license_mask = xcontent_device->license_mask();

    data.set_file_name(base_path.filename().string());
    fwrite(&data, 1, sizeof(kernel::xam::XCONTENT_AGGREGATE_DATA), file);
    fwrite(&license_mask, 1, sizeof(license_mask), file);
    fclose(file);
  }
  return;
}
}  // namespace vfs
}  // namespace xe

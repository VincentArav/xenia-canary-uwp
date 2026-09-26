/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_device.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {
namespace {

bool IsSafeGuestComponent(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && name.size() <= 255 &&
         name.find('\\') == std::string_view::npos &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

}  // namespace

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path,
                               bool read_only,
                               std::vector<std::string> root_entry_allowlist,
                               TraversalPolicy traversal_policy,
                               std::filesystem::path writable_root_overlay)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      writable_root_overlay_(std::move(writable_root_overlay)),
      root_entry_allowlist_(std::move(root_entry_allowlist)),
      traversal_policy_(traversal_policy),
      read_only_(read_only) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      XELOGE("Host path does not exist");
      return false;
    }
  }
  if (traversal_policy_ == TraversalPolicy::kSafeReadOnlyTree) {
    std::error_code root_status_error;
    const std::filesystem::file_status root_status =
        std::filesystem::symlink_status(host_path_, root_status_error);
    if (!read_only_ || root_status_error ||
        filesystem::IsSymlinkOrReparsePoint(host_path_) ||
        !std::filesystem::is_directory(root_status)) {
      XELOGE("Unsafe host path root rejected: {}", path_to_utf8(host_path_));
      return false;
    }
  }

  const bool has_root_overlay = !writable_root_overlay_.empty();
  if (has_root_overlay) {
    // Only an allowlisted safe read-only tree may have a root overlay, and the
    // two host trees must be disjoint.
    if (!read_only_ ||
        traversal_policy_ != TraversalPolicy::kSafeReadOnlyTree ||
        root_entry_allowlist_.empty() ||
        filesystem::HostPathsOverlap(host_path_, writable_root_overlay_)) {
      XELOGE("Writable root overlay rejected: {}",
             path_to_utf8(writable_root_overlay_));
      return false;
    }
    std::error_code create_error;
    std::filesystem::create_directories(writable_root_overlay_, create_error);
    std::error_code overlay_status_error;
    const std::filesystem::file_status overlay_status =
        std::filesystem::symlink_status(writable_root_overlay_,
                                        overlay_status_error);
    if (create_error || overlay_status_error ||
        filesystem::IsSymlinkOrReparsePoint(writable_root_overlay_) ||
        !std::filesystem::is_directory(overlay_status)) {
      XELOGE("Unsafe writable root overlay rejected: {}",
             path_to_utf8(writable_root_overlay_));
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  if (has_root_overlay) {
    // The root stays read-only (its host path is the read-only tree) but
    // creates new children in the overlay.
    root_entry->create_host_path_ = writable_root_overlay_;
  }
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  safe_tree_entry_count_ = 0;
  if (!PopulateEntry(root_entry, host_path_, false, 0) ||
      (has_root_overlay &&
       !PopulateEntry(root_entry, writable_root_overlay_, true, 0))) {
    root_entry_.reset();
    return false;
  }

  return true;
}

void HostPathDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("HostPathDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

bool HostPathDevice::PopulateEntry(HostPathEntry* parent_entry,
                                   const std::filesystem::path& host_directory,
                                   bool writable_overlay, size_t depth) {
  if (traversal_policy_ == TraversalPolicy::kSafeReadOnlyTree &&
      depth > kSafeTreeMaximumDepth) {
    XELOGE("Host path tree exceeds the maximum safe depth");
    return false;
  }
  std::vector<filesystem::FileInfo> child_infos =
      filesystem::ListFiles(host_directory);
  if (!writable_overlay && parent_entry == root_entry_.get() &&
      !root_entry_allowlist_.empty()) {
    child_infos.erase(
        std::remove_if(
            child_infos.begin(), child_infos.end(),
            [this](const filesystem::FileInfo& child_info) {
              const std::string child_name = path_to_utf8(child_info.name);
              const auto allowed = std::find_if(
                  root_entry_allowlist_.cbegin(), root_entry_allowlist_.cend(),
                  [&child_name](std::string_view allowed_name) {
                    return child_name == allowed_name;
                  });
              return allowed == root_entry_allowlist_.cend();
            }),
        child_infos.end());
  }

  if (traversal_policy_ == TraversalPolicy::kSafeReadOnlyTree) {
    if (child_infos.size() > kSafeTreeMaximumEntries - safe_tree_entry_count_) {
      XELOGE("Host path tree exceeds the maximum safe entry count");
      return false;
    }
    safe_tree_entry_count_ += child_infos.size();

    const std::filesystem::path& tree_root =
        writable_overlay ? writable_root_overlay_ : host_path_;
    std::unordered_set<std::string> normalized_child_names;
    normalized_child_names.reserve(child_infos.size());
    for (size_t index = 0; index < child_infos.size(); ++index) {
      const filesystem::FileInfo& child_info = child_infos[index];
      const std::filesystem::path child_path = host_directory / child_info.name;
      const std::string child_name = path_to_utf8(child_info.name);
      if (!utf8::is_valid(child_name) || !IsSafeGuestComponent(child_name)) {
        XELOGE("Unsafe host path entry name rejected: {}",
               path_to_utf8(child_path));
        return false;
      }
      // An overlay entry must not shadow, or be shadowed by, an existing
      // read-only sibling.
      if (!normalized_child_names.insert(utf8::lower_ascii(child_name))
               .second ||
          parent_entry->GetChild(child_name)) {
        XELOGE("Case-colliding host path entry rejected: {}", child_name);
        return false;
      }
      const std::string relative_path =
          path_to_utf8(child_path.lexically_relative(tree_root));
      std::error_code child_status_error;
      const std::filesystem::file_status child_status =
          std::filesystem::symlink_status(child_path, child_status_error);
      const bool is_directory = std::filesystem::is_directory(child_status);
      const bool is_regular_file =
          std::filesystem::is_regular_file(child_status);
      if (relative_path.empty() ||
          relative_path.size() > kSafeTreeMaximumRelativePathBytes ||
          child_status_error || child_info.is_symlink_or_reparse_point ||
          filesystem::IsSymlinkOrReparsePoint(child_path) ||
          (!is_directory && !is_regular_file) ||
          (is_directory !=
           (child_info.type == filesystem::FileInfo::Type::kDirectory))) {
        XELOGE("Unsafe host path entry rejected: {}", path_to_utf8(child_path));
        return false;
      }
    }
  }

  for (filesystem::FileInfo& child_info : child_infos) {
    auto child = HostPathEntry::Create(this, parent_entry,
                                       host_directory / child_info.name,
                                       child_info, writable_overlay);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    // Never recurse through a link (FileInfo may describe its target): it could
    // leave the tree or loop.
    if (child_info.type == xe::filesystem::FileInfo::Type::kDirectory &&
        !child_info.is_symlink_or_reparse_point) {
      if (!PopulateEntry(child, child->host_path(), writable_overlay,
                         depth + 1)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace vfs
}  // namespace xe

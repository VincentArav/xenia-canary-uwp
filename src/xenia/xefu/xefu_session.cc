/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_session.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe::xefu {
namespace {

XefuSessionMountResult MakeError(XefuSessionMountError error) {
  XefuSessionMountResult result;
  result.error = error;
  return result;
}

std::string MakeCompatibilityPath(std::string_view filename) {
  return std::string(kXefuCompatibilityDevice) + "\\" + std::string(filename);
}

std::string MakeConfigPath(std::string_view filename) {
  return std::string(kXefuConfigDevice) + "\\" + std::string(filename);
}

std::string MakeConfigLoaderPath(uint32_t title_id) {
  return std::string(kXefuConfigLoaderSymbolicLink) + "\\Configs\\" +
         XefuConfigFilenameForTitleId(title_id);
}

bool IsGuestFilename(std::string_view filename) {
  return !filename.empty() && filename != "." && filename != ".." &&
         filename.find('\\') == std::string_view::npos &&
         filename.find('/') == std::string_view::npos &&
         filename.find('\0') == std::string_view::npos;
}

bool RevalidateFile(const std::filesystem::path& path, uint64_t expected_size,
                    const Sha1Digest& expected_sha1,
                    const std::optional<Sha256Digest>& expected_sha256) {
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, status_error);
  if (status_error || std::filesystem::is_symlink(status) ||
      filesystem::IsSymlinkOrReparsePoint(path) ||
      !std::filesystem::is_regular_file(status)) {
    return false;
  }

  const std::optional<filesystem::FileInfo> info = filesystem::GetInfo(path);
  if (!info || info->type != filesystem::FileInfo::Type::kFile ||
      uint64_t(info->total_size) != expected_size) {
    return false;
  }

  XefuFileDigests actual_digests;
  return ComputeFileDigests(path, &actual_digests) &&
         actual_digests.size == expected_size &&
         actual_digests.sha1 == expected_sha1 &&
         (!expected_sha256 || actual_digests.sha256 == *expected_sha256);
}

}  // namespace

XefuSessionMountResult MountPreparedXefuSession(
    vfs::VirtualFileSystem* file_system,
    const PreparedXefuLaunch& prepared_launch) {
  if (!file_system) {
    return MakeError(XefuSessionMountError::kInvalidFileSystem);
  }

  const XefuBuildDescriptor* const build_descriptor =
      FindKnownXefuBuild(prepared_launch.assets.build);
  const std::optional<XefuConfigPackFlavor> required_config_pack =
      build_descriptor ? build_descriptor->required_external_config_pack
                       : std::nullopt;
  const std::string config_loader_guest_path =
      required_config_pack ? MakeConfigLoaderPath(prepared_launch.xbe.title_id)
                           : std::string();

  const ValidatedXefuAsset* runtime =
      prepared_launch.assets.Find(XefuAssetRole::kRuntime);
  if (!runtime) {
    return MakeError(XefuSessionMountError::kMissingRuntime);
  }
  if (!IsGuestFilename(runtime->guest_filename)) {
    return MakeError(XefuSessionMountError::kInvalidGuestFilename);
  }
  if (const ValidatedXefuAsset* title_module =
          prepared_launch.assets.Find(XefuAssetRole::kTitleModule);
      title_module && !IsGuestFilename(title_module->guest_filename)) {
    return MakeError(XefuSessionMountError::kInvalidGuestFilename);
  }

  // Only a config-loader build takes a config, and it may go without one.
  if (prepared_launch.external_config && !required_config_pack) {
    return MakeError(XefuSessionMountError::kConfigPathMismatch);
  }

  std::string config_host_filename;
  std::string config_absolute_guest_path;
  std::string config_hddx_guest_path;
  if (prepared_launch.external_config) {
    const ValidatedXefuConfig& config = *prepared_launch.external_config;
    const std::filesystem::path normalized_config_path =
        config.path.lexically_normal();
    const std::filesystem::path expected_config_root =
        (prepared_launch.assets.root / "Configs").lexically_normal();
    const std::string expected_config_filename =
        XefuConfigFilenameForTitleId(prepared_launch.xbe.title_id);
    config_host_filename = path_to_utf8(normalized_config_path.filename());
    if (!required_config_pack || config.source_pack != *required_config_pack ||
        config.title_id != prepared_launch.xbe.title_id ||
        config.size != kXefuConfigSize ||
        normalized_config_path.parent_path() != expected_config_root ||
        config_host_filename != expected_config_filename ||
        config.canonical_guest_filename != expected_config_filename ||
        !IsGuestFilename(config_host_filename)) {
      return MakeError(XefuSessionMountError::kConfigPathMismatch);
    }
    if (!RevalidateFile(config.path, config.size, config.sha1, config.sha256)) {
      return MakeError(XefuSessionMountError::kConfigChanged);
    }
    config_absolute_guest_path =
        MakeConfigPath(config.canonical_guest_filename);
    config_hddx_guest_path = std::string(kXefuHddxSymbolicLink) +
                             "\\Compatibility\\Configs\\" +
                             config.canonical_guest_filename;
  }

  std::vector<std::string> compatibility_allowlist;
  compatibility_allowlist.reserve(prepared_launch.assets.assets.size());
  const std::filesystem::path normalized_asset_root =
      prepared_launch.assets.root.lexically_normal();
  for (const ValidatedXefuAsset& asset : prepared_launch.assets.assets) {
    const std::filesystem::path normalized_asset_path =
        asset.path.lexically_normal();
    if (normalized_asset_path.parent_path() != normalized_asset_root) {
      return MakeError(XefuSessionMountError::kAssetRootMismatch);
    }
    const std::string host_filename =
        path_to_utf8(normalized_asset_path.filename());
    if (!IsGuestFilename(host_filename)) {
      return MakeError(XefuSessionMountError::kInvalidGuestFilename);
    }
    compatibility_allowlist.push_back(host_filename);
  }

  constexpr std::string_view kMediaPrefix = "\\Device\\Cdrom0\\";
  if (!prepared_launch.guest_xbe_path.starts_with(kMediaPrefix)) {
    return MakeError(XefuSessionMountError::kGuestMediaPathMismatch);
  }

  if (!RevalidateFile(prepared_launch.xbe_host_path, prepared_launch.xbe_size,
                      prepared_launch.xbe_sha1, std::nullopt)) {
    return MakeError(XefuSessionMountError::kXbeChanged);
  }

  // Resolve the selected media file to one exact native directory entry before
  // constructing the full safe read-only media tree. The tree builder also
  // rejects case-insensitive collisions globally; this early check preserves a
  // precise error for ambiguity in the selected XBE itself.
  std::vector<std::string> native_media_names;
  const std::string selected_media_name =
      path_to_utf8(prepared_launch.xbe_host_path.filename());
  for (const filesystem::FileInfo& entry :
       filesystem::ListFiles(prepared_launch.xbe_host_path.parent_path())) {
    const std::string entry_name = path_to_utf8(entry.name);
    if (utf8::equal_case(entry_name, selected_media_name)) {
      native_media_names.push_back(entry_name);
    }
  }
  if (native_media_names.size() != 1) {
    return MakeError(XefuSessionMountError::kMediaFilenameAmbiguous);
  }
  for (const ValidatedXefuAsset& asset : prepared_launch.assets.assets) {
    if (!RevalidateFile(asset.path, asset.size, asset.sha1, asset.sha256)) {
      return MakeError(XefuSessionMountError::kAssetChanged);
    }
  }
  const std::string_view guest_media_name =
      std::string_view(prepared_launch.guest_xbe_path)
          .substr(kMediaPrefix.size());
  if (guest_media_name.find('\\') != std::string_view::npos ||
      !utf8::equal_case(
          guest_media_name,
          path_to_utf8(prepared_launch.xbe_host_path.filename()))) {
    return MakeError(XefuSessionMountError::kGuestMediaPathMismatch);
  }

  // XeFu writes files in the system-partition Compatibility folder
  // (X:\config.bin) and in Partition1\Compatibility (its Y: link). Both live
  // under the writable root, apart from the read-only trees.
  std::filesystem::path writable_compatibility_overlay;
  std::filesystem::path writable_partition1_compatibility;
  if (!prepared_launch.writable_root.empty()) {
    const std::filesystem::path writable_root =
        prepared_launch.writable_root.lexically_normal();
    if (!writable_root.is_absolute()) {
      return MakeError(XefuSessionMountError::kWritableStorageMountFailed);
    }
    writable_compatibility_overlay =
        writable_root / "SystemPartition" / "Compatibility";
    writable_partition1_compatibility =
        writable_root / "Partition1" / "Compatibility";
    for (const std::filesystem::path& writable_path :
         {writable_compatibility_overlay, writable_partition1_compatibility}) {
      if (filesystem::HostPathsOverlap(writable_path,
                                       prepared_launch.assets.root) ||
          filesystem::HostPathsOverlap(
              writable_path, prepared_launch.xbe_host_path.parent_path()) ||
          (prepared_launch.external_config &&
           filesystem::HostPathsOverlap(
               writable_path,
               prepared_launch.external_config->path.parent_path()))) {
        return MakeError(XefuSessionMountError::kWritableStorageMountFailed);
      }
    }
  }

  // Every device is initialized first; the group is then validated and
  // published atomically.
  vfs::VirtualFileSystem::MountGroup mount_group;
  if (prepared_launch.external_config) {
    auto config_device = std::make_unique<vfs::HostPathDevice>(
        kXefuConfigDevice, prepared_launch.external_config->path.parent_path(),
        true, std::vector<std::string>{config_host_filename},
        vfs::HostPathDevice::TraversalPolicy::kSafeReadOnlyTree);
    if (!config_device->Initialize()) {
      return MakeError(XefuSessionMountError::kConfigMountFailed);
    }
    mount_group.devices.push_back(std::move(config_device));
  }

  auto compatibility_device = std::make_unique<vfs::HostPathDevice>(
      kXefuCompatibilityDevice, prepared_launch.assets.root, true,
      std::move(compatibility_allowlist),
      vfs::HostPathDevice::TraversalPolicy::kSafeReadOnlyTree,
      writable_compatibility_overlay);
  if (!compatibility_device->Initialize()) {
    return MakeError(XefuSessionMountError::kCompatibilityMountFailed);
  }
  mount_group.devices.push_back(std::move(compatibility_device));

  if (!writable_partition1_compatibility.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(writable_partition1_compatibility,
                                        create_error);
    if (create_error || filesystem::IsSymlinkOrReparsePoint(
                            writable_partition1_compatibility)) {
      return MakeError(XefuSessionMountError::kWritableStorageMountFailed);
    }
    auto partition1_device = std::make_unique<vfs::HostPathDevice>(
        kXefuPartition1CompatibilityDevice, writable_partition1_compatibility,
        false);
    if (!partition1_device->Initialize()) {
      return MakeError(XefuSessionMountError::kWritableStorageMountFailed);
    }
    mount_group.devices.push_back(std::move(partition1_device));
  }

  // The NullDevice CompleteLaunch adds for raw cache probes, in the group so it
  // is ordered after the more specific Compatibility and Configs devices.
  // CompleteLaunch's own registration is then rejected.
  auto raw_harddisk_device = std::make_unique<vfs::NullDevice>(
      std::string(kXefuBroadHarddiskDevice),
      std::initializer_list<std::string>{"\\Partition0", "\\Cache0",
                                         "\\Cache1"});
  if (!raw_harddisk_device->Initialize()) {
    return MakeError(XefuSessionMountError::kCompatibilityMountFailed);
  }
  mount_group.devices.push_back(std::move(raw_harddisk_device));

  auto media_device = std::make_unique<vfs::HostPathDevice>(
      kXefuMediaDevice, prepared_launch.xbe_host_path.parent_path(), true,
      std::vector<std::string>{},
      vfs::HostPathDevice::TraversalPolicy::kSafeReadOnlyTree);
  if (!media_device->Initialize()) {
    return MakeError(XefuSessionMountError::kMediaMountFailed);
  }
  mount_group.devices.push_back(std::move(media_device));

  mount_group.symbolic_links.push_back({std::string(kXefuHddxSymbolicLink),
                                        std::string(kXefuSystemPartitionPath)});
  // The selector launches the runtime with the XBE's directory as its mount
  // root, and the runtime opens the game as d:\<xbe name> without creating D:
  // itself. GAME: and D: therefore name the media tree, not Compatibility; the
  // runtime is loaded through its absolute Compatibility path instead.
  mount_group.symbolic_links.push_back(
      {std::string(kXefuGameSymbolicLink), std::string(kXefuMediaDevice)});
  mount_group.symbolic_links.push_back(
      {std::string(kXefuPartitionSymbolicLink), std::string(kXefuMediaDevice)});
  if (required_config_pack) {
    mount_group.symbolic_links.push_back(
        {std::string(kXefuConfigLoaderSymbolicLink),
         std::string(kXefuCompatibilityDevice)});
  }

  // An asset whose host filename differs from the name guest imports use gets
  // a file alias inside the Compatibility device.
  for (const ValidatedXefuAsset& asset : prepared_launch.assets.assets) {
    const std::string host_filename = path_to_utf8(asset.path.filename());
    if (!IsGuestFilename(host_filename) ||
        !IsGuestFilename(asset.guest_filename)) {
      return MakeError(XefuSessionMountError::kInvalidGuestFilename);
    }
    if (!utf8::equal_case(host_filename, asset.guest_filename)) {
      mount_group.symbolic_links.push_back(
          {MakeCompatibilityPath(asset.guest_filename),
           MakeCompatibilityPath(host_filename)});
    }
  }

  auto mounts = std::make_unique<vfs::ScopedVfsMount>(file_system);
  if (!mounts->RegisterMountGroup(std::move(mount_group))) {
    return MakeError(XefuSessionMountError::kMountOrderConflict);
  }

  vfs::Entry* selected_config_entry = nullptr;
  if (prepared_launch.external_config) {
    selected_config_entry =
        file_system->ResolvePath(config_absolute_guest_path);
    if (!selected_config_entry) {
      return MakeError(XefuSessionMountError::kConfigMountFailed);
    }
  }
  if (!file_system->ResolvePath(prepared_launch.guest_xbe_path)) {
    return MakeError(XefuSessionMountError::kMediaMountFailed);
  }
  if (required_config_pack &&
      file_system->ResolvePath(config_loader_guest_path) !=
          selected_config_entry) {
    return MakeError(XefuSessionMountError::kConfigMountFailed);
  }
  if (prepared_launch.external_config &&
      file_system->ResolvePath(config_hddx_guest_path) !=
          selected_config_entry) {
    return MakeError(XefuSessionMountError::kConfigMountFailed);
  }

  auto session = std::make_unique<MountedXefuSession>();
  session->mounts = std::move(mounts);
  session->nested_title_id = prepared_launch.xbe.title_id;
  session->runtime_host_path = runtime->path;
  session->runtime_guest_path = MakeCompatibilityPath(runtime->guest_filename);
  if (const ValidatedXefuAsset* title_module =
          prepared_launch.assets.Find(XefuAssetRole::kTitleModule)) {
    session->title_module_guest_path =
        MakeCompatibilityPath(title_module->guest_filename);
  }
  session->xbe_guest_path = prepared_launch.guest_xbe_path;
  if (prepared_launch.external_config) {
    session->config_guest_path = config_loader_guest_path;
  }

  XefuSessionMountResult result;
  result.session = std::move(session);
  return result;
}

std::string_view XefuSessionMountErrorToString(XefuSessionMountError error) {
  switch (error) {
    case XefuSessionMountError::kNone:
      return "none";
    case XefuSessionMountError::kInvalidFileSystem:
      return "the virtual filesystem is unavailable";
    case XefuSessionMountError::kMissingRuntime:
      return "the prepared XeFu launch has no runtime";
    case XefuSessionMountError::kInvalidGuestFilename:
      return "a prepared XeFu asset has an invalid guest filename";
    case XefuSessionMountError::kAssetRootMismatch:
      return "a prepared XeFu asset is outside the compatibility root";
    case XefuSessionMountError::kGuestMediaPathMismatch:
      return "the guest XBE path does not match the selected host XBE";
    case XefuSessionMountError::kMediaFilenameAmbiguous:
      return "the selected XBE filename is ambiguous on the host filesystem";
    case XefuSessionMountError::kXbeChanged:
      return "the selected XBE changed after launch preparation";
    case XefuSessionMountError::kAssetChanged:
      return "a XeFu asset changed after launch preparation";
    case XefuSessionMountError::kConfigPathMismatch:
      return "the prepared external XeFu config does not match the title and "
             "build";
    case XefuSessionMountError::kConfigChanged:
      return "the external XeFu config changed after launch preparation";
    case XefuSessionMountError::kMountOrderConflict:
      return "an existing device or alias shadows a namespace required by "
             "XeFu";
    case XefuSessionMountError::kConfigMountFailed:
      return "the selected external XeFu config could not be mounted";
    case XefuSessionMountError::kCompatibilityMountFailed:
      return "the XeFu compatibility directory could not be mounted";
    case XefuSessionMountError::kMediaMountFailed:
      return "the original-Xbox media directory could not be mounted";
    case XefuSessionMountError::kSymbolicLinkFailed:
      return "a required XeFu device alias could not be registered";
    case XefuSessionMountError::kWritableStorageMountFailed:
      return "XeFu writable storage could not be mounted safely";
  }
  return "unknown XeFu session mount error";
}

}  // namespace xe::xefu

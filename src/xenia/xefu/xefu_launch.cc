/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_launch.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "third_party/crypto/TinySHA1.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/xefu/xefu_media.h"

namespace xe::xefu {
namespace {

void WriteBe32(std::array<uint8_t, kXefuLaunchDataSize>& bytes, size_t offset,
               uint32_t value) {
  bytes[offset] = uint8_t(value >> 24);
  bytes[offset + 1] = uint8_t(value >> 16);
  bytes[offset + 2] = uint8_t(value >> 8);
  bytes[offset + 3] = uint8_t(value);
}

}  // namespace

XefuLaunchDataResult SerializeXefuLaunchData(
    const XefuLaunchDataParams& params) {
  XefuLaunchDataResult result;
  if (params.guest_xbe_path.empty()) {
    result.error = XefuLaunchDataError::kEmptyPath;
    return result;
  }
  if (params.guest_xbe_path.find('\0') != std::string::npos) {
    result.error = XefuLaunchDataError::kEmbeddedNull;
    return result;
  }

  std::string normalized_path = params.guest_xbe_path;
  std::replace(normalized_path.begin(), normalized_path.end(), '/', '\\');
  if (normalized_path.size() >= kXefuLaunchDataPathCapacity) {
    result.error = XefuLaunchDataError::kPathTooLong;
    return result;
  }
  if (!normalized_path.starts_with("\\Device\\")) {
    result.error = XefuLaunchDataError::kNonAbsoluteDevicePath;
    return result;
  }

  size_t component_count = 0;
  size_t component_start = 1;
  while (component_start <= normalized_path.size()) {
    const size_t component_end = normalized_path.find('\\', component_start);
    const size_t end = component_end == std::string::npos
                           ? normalized_path.size()
                           : component_end;
    const std::string_view component(normalized_path.data() + component_start,
                                     end - component_start);
    if (component == "." || component == "..") {
      result.error = XefuLaunchDataError::kTraversalComponent;
      return result;
    }
    if (component.empty()) {
      result.error = XefuLaunchDataError::kNonCanonicalPath;
      return result;
    }
    ++component_count;
    if (component_end == std::string::npos) {
      break;
    }
    component_start = component_end + 1;
  }
  // Device, the device object, and at least one path component are required.
  if (component_count < 3) {
    result.error = XefuLaunchDataError::kNonCanonicalPath;
    return result;
  }

  WriteBe32(result.bytes, 0, params.source);
  WriteBe32(result.bytes, 4, uint32_t(params.reason));
  WriteBe32(result.bytes, 8, params.live_enabled);
  std::memcpy(result.bytes.data() + kXefuLaunchDataPathOffset,
              normalized_path.data(), normalized_path.size());
  result.normalized_guest_xbe_path = std::move(normalized_path);
  return result;
}

std::string_view XefuLaunchDataErrorToString(XefuLaunchDataError error) {
  switch (error) {
    case XefuLaunchDataError::kNone:
      return "none";
    case XefuLaunchDataError::kEmptyPath:
      return "the guest XBE path is empty";
    case XefuLaunchDataError::kEmbeddedNull:
      return "the guest XBE path contains an embedded NUL";
    case XefuLaunchDataError::kPathTooLong:
      return "the guest XBE path does not fit in launch data";
    case XefuLaunchDataError::kNonAbsoluteDevicePath:
      return "the guest XBE path is not an absolute \\Device path";
    case XefuLaunchDataError::kTraversalComponent:
      return "the guest XBE path contains a traversal component";
    case XefuLaunchDataError::kNonCanonicalPath:
      return "the guest XBE path is not canonical";
  }
  return "unknown XeFu launch-data error";
}

PrepareXefuLaunchResult PrepareXefuLaunch(const XefuLaunchRequest& request) {
  const XefuBuildDescriptor* descriptor = FindKnownXefuBuild(request.build);
  if (!descriptor) {
    PrepareXefuLaunchResult result;
    result.error = XefuPrepareError::kInvalidAssets;
    result.asset_error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.problem_path = request.compatibility_root;
    return result;
  }
  return PrepareXefuLaunch(request, *descriptor);
}

PrepareXefuLaunchResult PrepareXefuLaunch(
    const XefuLaunchRequest& request, const XefuBuildDescriptor& descriptor) {
  PrepareXefuLaunchResult result;

  // The request must name the descriptor's build.
  if (request.build != descriptor.id) {
    result.error = XefuPrepareError::kInvalidAssets;
    result.asset_error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.problem_path = request.compatibility_root;
    return result;
  }

  // Parse and hash the same bytes.
  std::vector<uint8_t> xbe_snapshot;
  if (!request.media_image_path.empty()) {
    // The same checks as a loose XBE: one regular file, not a link.
    std::error_code image_status_error;
    const std::filesystem::file_status image_status =
        std::filesystem::symlink_status(request.media_image_path,
                                        image_status_error);
    if (image_status_error || !std::filesystem::is_regular_file(image_status) ||
        filesystem::IsSymlinkOrReparsePoint(request.media_image_path)) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error = std::filesystem::exists(request.media_image_path)
                             ? XbeMetadataError::kReadFailed
                             : XbeMetadataError::kFileNotFound;
      return result;
    }
    XefuDiscImageXbe disc_xbe = ReadXefuDiscImageXbe(request.media_image_path);
    if (!disc_xbe) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error = disc_xbe.error;
      return result;
    }
    xbe_snapshot = std::move(disc_xbe.bytes);
  } else {
    std::error_code xbe_status_error;
    const std::filesystem::file_status xbe_status =
        std::filesystem::symlink_status(request.xbe_host_path,
                                        xbe_status_error);
    if (xbe_status_error || !std::filesystem::is_regular_file(xbe_status) ||
        std::filesystem::is_symlink(xbe_status) ||
        filesystem::IsSymlinkOrReparsePoint(request.xbe_host_path)) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error =
          xbe_status_error == std::errc::no_such_file_or_directory ||
                  (!xbe_status_error &&
                   xbe_status.type() == std::filesystem::file_type::not_found)
              ? XbeMetadataError::kFileNotFound
              : XbeMetadataError::kReadFailed;
      return result;
    }
    std::unique_ptr<MappedMemory> xbe_mapping =
        MappedMemory::Open(request.xbe_host_path, MappedMemory::Mode::kRead);
    if (!xbe_mapping) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error = filesystem::GetInfo(request.xbe_host_path)
                             ? XbeMetadataError::kReadFailed
                             : XbeMetadataError::kFileNotFound;
      return result;
    }
    // Another process can change a mapping's pages, so parse and hash a copy;
    // mounting rejects a file changed after it.
    if (xbe_mapping->size() > xbe_snapshot.max_size()) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error = XbeMetadataError::kReadFailed;
      return result;
    }
    try {
      xbe_snapshot.assign(xbe_mapping->data(),
                          xbe_mapping->data() + xbe_mapping->size());
    } catch (const std::bad_alloc&) {
      result.error = XefuPrepareError::kInvalidXbe;
      result.xbe_error = XbeMetadataError::kReadFailed;
      return result;
    }
  }
  const std::span<const uint8_t> xbe_bytes(xbe_snapshot);
  XbeMetadataResult xbe_result =
      ParseXbeMetadata(xbe_bytes, XbeValidationMode::kCanonicalLaunch);
  if (!xbe_result) {
    result.error = XefuPrepareError::kInvalidXbe;
    result.xbe_error = xbe_result.error;
    return result;
  }

  // The config's filename comes from the XBE's title ID.
  XefuConfigValidationResult config_result = ValidateXefuConfig(
      request.compatibility_root, xbe_result.metadata.title_id,
      request.external_config, descriptor.required_external_config_pack);
  if (!config_result) {
    result.error = XefuPrepareError::kInvalidExternalConfig;
    result.config_error = config_result.error;
    result.config_validation = std::move(config_result);
    return result;
  }

  if (!descriptor.capabilities.preparation_supported) {
    result.error = XefuPrepareError::kInvalidAssets;
    result.asset_error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.error = XefuAssetError::kUnsupportedBuild;
    result.asset_validation.problem_path = request.compatibility_root;
    return result;
  }

  XefuLaunchDataParams launch_params;
  launch_params.reason = request.reason;
  launch_params.live_enabled = request.live_enabled;
  launch_params.guest_xbe_path = request.xbe_guest_path;
  XefuLaunchDataResult launch_data_result =
      SerializeXefuLaunchData(launch_params);
  if (!launch_data_result) {
    result.error = XefuPrepareError::kInvalidLaunchData;
    result.launch_data_error = launch_data_result.error;
    return result;
  }

  Sha1Digest xbe_sha1{};
  sha1::SHA1 xbe_hasher;
  xbe_hasher.processBytes(xbe_bytes.data(), xbe_bytes.size());
  xbe_hasher.finalize(xbe_sha1.data());

  // Assets are hashed last, closest to mounting, which checks them again.
  XefuAssetValidationResult asset_result =
      ValidateXefuAssets(request.compatibility_root, descriptor);
  if (!asset_result) {
    result.error = XefuPrepareError::kInvalidAssets;
    result.asset_error = asset_result.error;
    result.asset_validation = std::move(asset_result);
    return result;
  }

  result.launch.xbe_host_path = request.xbe_host_path;
  result.launch.media_image_path = request.media_image_path;
  result.launch.guest_xbe_path =
      std::move(launch_data_result.normalized_guest_xbe_path);
  result.launch.xbe_sha1 = xbe_sha1;
  result.launch.xbe_size = xbe_bytes.size();
  result.launch.xbe = std::move(xbe_result.metadata);
  result.launch.assets = std::move(asset_result.assets);
  if (request.external_config) {
    result.launch.external_config = std::move(config_result.config);
  }
  result.launch.launch_data = launch_data_result.bytes;
  result.launch.writable_root = request.writable_root;
  return result;
}

}  // namespace xe::xefu

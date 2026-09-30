/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_LAUNCH_H_
#define XENIA_XEFU_XEFU_LAUNCH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "xenia/xefu/xbe_metadata.h"
#include "xenia/xefu/xefu_assets.h"
#include "xenia/xefu/xefu_config.h"

namespace xe::xefu {

inline constexpr size_t kXefuLaunchDataSize = 0x3FC;
inline constexpr size_t kXefuLaunchDataPathOffset = 0x1FC;
inline constexpr size_t kXefuLaunchDataPathCapacity = 0x200;

enum class XefuLaunchReason : uint32_t {
  kInitial = 0,
  kExitOrRestart = 6,
};

enum class XefuLaunchDataError {
  kNone,
  kEmptyPath,
  kEmbeddedNull,
  kPathTooLong,
  kNonAbsoluteDevicePath,
  kTraversalComponent,
  kNonCanonicalPath,
};

struct XefuLaunchDataParams {
  uint32_t source = 2;
  XefuLaunchReason reason = XefuLaunchReason::kInitial;
  uint32_t live_enabled = 0;
  std::string guest_xbe_path;
};

struct XefuLaunchDataResult {
  XefuLaunchDataError error = XefuLaunchDataError::kNone;
  std::array<uint8_t, kXefuLaunchDataSize> bytes{};
  std::string normalized_guest_xbe_path;

  explicit operator bool() const { return error == XefuLaunchDataError::kNone; }
};

XefuLaunchDataResult SerializeXefuLaunchData(
    const XefuLaunchDataParams& params);
std::string_view XefuLaunchDataErrorToString(XefuLaunchDataError error);

struct XefuLaunchRequest {
  std::filesystem::path xbe_host_path;
  // An original Xbox disc image (an XISO, as xemu plays, or a full dump) to
  // mount as the disc instead of the XBE's folder. The game is then its
  // default.xbe and xbe_host_path is unused.
  std::filesystem::path media_image_path;
  std::string xbe_guest_path;
  std::filesystem::path compatibility_root;
  XefuBuildId build = XefuBuildId::kRetail7b;
  XefuLaunchReason reason = XefuLaunchReason::kInitial;
  uint32_t live_enabled = 0;
  std::optional<XefuConfigRequest> external_config;
  // Host directory for XeFu's own writable files: the overlay of the
  // system-partition Compatibility folder and the Partition1 Compatibility
  // folder. Empty for none.
  std::filesystem::path writable_root;
};

enum class XefuPrepareError {
  kNone,
  kInvalidXbe,
  kInvalidAssets,
  kInvalidExternalConfig,
  kInvalidLaunchData,
};

struct PreparedXefuLaunch {
  std::filesystem::path xbe_host_path;
  // Set for a disc image launch; see XefuLaunchRequest.
  std::filesystem::path media_image_path;
  std::string guest_xbe_path;
  Sha1Digest xbe_sha1{};
  uint64_t xbe_size = 0;
  XbeMetadata xbe;
  ValidatedXefuAssetSet assets;
  std::optional<ValidatedXefuConfig> external_config;
  std::array<uint8_t, kXefuLaunchDataSize> launch_data{};
  std::filesystem::path writable_root;
};

struct PrepareXefuLaunchResult {
  XefuPrepareError error = XefuPrepareError::kNone;
  XbeMetadataError xbe_error = XbeMetadataError::kNone;
  XefuAssetError asset_error = XefuAssetError::kNone;
  XefuAssetValidationResult asset_validation;
  XefuConfigError config_error = XefuConfigError::kNone;
  XefuConfigValidationResult config_validation;
  XefuLaunchDataError launch_data_error = XefuLaunchDataError::kNone;
  PreparedXefuLaunch launch;

  explicit operator bool() const { return error == XefuPrepareError::kNone; }
};

PrepareXefuLaunchResult PrepareXefuLaunch(const XefuLaunchRequest& request);
PrepareXefuLaunchResult PrepareXefuLaunch(
    const XefuLaunchRequest& request, const XefuBuildDescriptor& descriptor);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_LAUNCH_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_SESSION_H_
#define XENIA_XEFU_XEFU_SESSION_H_

#include <memory>
#include <string>
#include <string_view>

#include "xenia/vfs/scoped_vfs_mount.h"
#include "xenia/xefu/xefu_launch.h"

namespace xe::vfs {
class VirtualFileSystem;
}  // namespace xe::vfs

namespace xe::xefu {

inline constexpr std::string_view kXefuCompatibilityDevice =
    "\\Device\\Harddisk0\\SystemPartition\\Compatibility";
inline constexpr std::string_view kXefuConfigDevice =
    "\\Device\\Harddisk0\\SystemPartition\\Compatibility\\Configs";
inline constexpr std::string_view kXefuBroadHarddiskDevice =
    "\\Device\\Harddisk0";
inline constexpr std::string_view kXefuSystemPartitionPath =
    "\\Device\\Harddisk0\\SystemPartition";
inline constexpr std::string_view kXefuMediaDevice = "\\Device\\Cdrom0";
// XeFu links Y: here for its per-console writable data.
inline constexpr std::string_view kXefuPartition1CompatibilityDevice =
    "\\Device\\Harddisk0\\Partition1\\Compatibility";
inline constexpr std::string_view kXefuHddxSymbolicLink = "HddX:";
inline constexpr std::string_view kXefuGameSymbolicLink = "GAME:";
inline constexpr std::string_view kXefuPartitionSymbolicLink = "D:";
// The config-loader runtime opens X:\\Configs\\{TITLEID}.bin. Separate from
// HddX:, whose root is the whole system partition.
inline constexpr std::string_view kXefuConfigLoaderSymbolicLink = "X:";

enum class XefuSessionMountError {
  kNone,
  kInvalidFileSystem,
  kMissingRuntime,
  kInvalidGuestFilename,
  kAssetRootMismatch,
  kGuestMediaPathMismatch,
  kMediaFilenameAmbiguous,
  kXbeChanged,
  kAssetChanged,
  kConfigPathMismatch,
  kConfigChanged,
  kMountOrderConflict,
  kConfigMountFailed,
  kCompatibilityMountFailed,
  kMediaMountFailed,
  kSymbolicLinkFailed,
  kWritableStorageMountFailed,
};

struct MountedXefuSession {
  std::unique_ptr<vfs::ScopedVfsMount> mounts;
  uint32_t nested_title_id = 0;
  std::filesystem::path runtime_host_path;
  std::string runtime_guest_path;
  std::string title_module_guest_path;
  std::string xbe_guest_path;
  std::string config_guest_path;
};

struct XefuSessionMountResult {
  XefuSessionMountError error = XefuSessionMountError::kNone;
  std::unique_ptr<MountedXefuSession> session;

  explicit operator bool() const {
    return error == XefuSessionMountError::kNone && session != nullptr;
  }
};

// Mounts a prepared launch's devices and links, after checking the XBE and
// assets again. Assets and media are read-only; a non-empty writable_root adds
// XeFu's writable Compatibility folders beneath it. Destroying the session
// unmounts the group.
XefuSessionMountResult MountPreparedXefuSession(
    vfs::VirtualFileSystem* file_system,
    const PreparedXefuLaunch& prepared_launch);

std::string_view XefuSessionMountErrorToString(XefuSessionMountError error);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_SESSION_H_

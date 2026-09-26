/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_LAUNCH_COORDINATOR_H_
#define XENIA_XEFU_XEFU_LAUNCH_COORDINATOR_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "xenia/xbox.h"
#include "xenia/xefu/xefu_session.h"

namespace xe::xefu {

// Writes launch data into XAM's loader storage and restores the previous bytes
// unless Commit is called. The storage must outlive this object.
class ScopedXefuLaunchData final {
 public:
  ScopedXefuLaunchData(std::vector<uint8_t>& loader_launch_data,
                       std::span<const uint8_t> launch_data);
  ~ScopedXefuLaunchData();

  ScopedXefuLaunchData(const ScopedXefuLaunchData&) = delete;
  ScopedXefuLaunchData& operator=(const ScopedXefuLaunchData&) = delete;

  void Commit() noexcept;
  void Rollback() noexcept;

 private:
  std::vector<uint8_t>* loader_launch_data_ = nullptr;
  std::vector<uint8_t> previous_launch_data_;
};

using XefuRuntimeLaunchCallback = std::function<X_STATUS(
    const std::filesystem::path& runtime_host_path,
    std::string_view runtime_guest_path, std::span<const uint8_t> launch_data)>;

struct CoordinatedXefuLaunchResult {
  XefuSessionMountError mount_error = XefuSessionMountError::kNone;
  X_STATUS runtime_status = X_STATUS_UNSUCCESSFUL;
  std::unique_ptr<MountedXefuSession> session;

  explicit operator bool() const {
    return mount_error == XefuSessionMountError::kNone &&
           XSUCCEEDED(runtime_status) && session != nullptr;
  }
};

// Mounts the session, then calls the launch callback. On success the caller
// owns the session for the title's lifetime; on failure the mounts are removed
// and the callback's status is returned.
CoordinatedXefuLaunchResult CoordinatePreparedXefuLaunch(
    vfs::VirtualFileSystem* file_system,
    const PreparedXefuLaunch& prepared_launch,
    const XefuRuntimeLaunchCallback& launch_runtime);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_LAUNCH_COORDINATOR_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_launch_coordinator.h"

#include <utility>

namespace xe::xefu {

ScopedXefuLaunchData::ScopedXefuLaunchData(
    std::vector<uint8_t>& loader_launch_data,
    std::span<const uint8_t> launch_data)
    : loader_launch_data_(&loader_launch_data),
      previous_launch_data_(loader_launch_data) {
  loader_launch_data.assign(launch_data.begin(), launch_data.end());
}

ScopedXefuLaunchData::~ScopedXefuLaunchData() { Rollback(); }

void ScopedXefuLaunchData::Commit() noexcept { loader_launch_data_ = nullptr; }

void ScopedXefuLaunchData::Rollback() noexcept {
  if (!loader_launch_data_) {
    return;
  }
  loader_launch_data_->swap(previous_launch_data_);
  loader_launch_data_ = nullptr;
}

CoordinatedXefuLaunchResult CoordinatePreparedXefuLaunch(
    vfs::VirtualFileSystem* file_system,
    const PreparedXefuLaunch& prepared_launch,
    const XefuRuntimeLaunchCallback& launch_runtime) {
  CoordinatedXefuLaunchResult result;
  if (!launch_runtime) {
    result.runtime_status = X_STATUS_INVALID_PARAMETER;
    return result;
  }

  XefuSessionMountResult mount_result =
      MountPreparedXefuSession(file_system, prepared_launch);
  if (!mount_result) {
    result.mount_error = mount_result.error;
    return result;
  }

  result.runtime_status = launch_runtime(
      mount_result.session->runtime_host_path,
      mount_result.session->runtime_guest_path, prepared_launch.launch_data);
  if (XFAILED(result.runtime_status)) {
    mount_result.session.reset();
    return result;
  }

  result.session = std::move(mount_result.session);
  return result;
}

}  // namespace xe::xefu

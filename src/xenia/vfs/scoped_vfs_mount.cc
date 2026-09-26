/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/scoped_vfs_mount.h"

#include <utility>

#include "xenia/vfs/virtual_file_system.h"

namespace xe::vfs {

ScopedVfsMount::ScopedVfsMount(VirtualFileSystem* file_system)
    : file_system_(file_system) {}

ScopedVfsMount::~ScopedVfsMount() { Reset(); }

ScopedVfsMount::ScopedVfsMount(ScopedVfsMount&& other) noexcept
    : file_system_(std::exchange(other.file_system_, nullptr)),
      mount_group_token_(
          std::exchange(other.mount_group_token_,
                        VirtualFileSystem::kInvalidRegistrationToken)),
      device_tokens_(std::move(other.device_tokens_)),
      symbolic_link_tokens_(std::move(other.symbolic_link_tokens_)) {
  other.device_tokens_.clear();
  other.symbolic_link_tokens_.clear();
}

ScopedVfsMount& ScopedVfsMount::operator=(ScopedVfsMount&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  Reset();
  file_system_ = std::exchange(other.file_system_, nullptr);
  mount_group_token_ = std::exchange(
      other.mount_group_token_, VirtualFileSystem::kInvalidRegistrationToken);
  device_tokens_ = std::move(other.device_tokens_);
  symbolic_link_tokens_ = std::move(other.symbolic_link_tokens_);
  other.device_tokens_.clear();
  other.symbolic_link_tokens_.clear();
  return *this;
}

bool ScopedVfsMount::MountDevice(std::unique_ptr<Device> device) {
  if (mount_group_token_ != VirtualFileSystem::kInvalidRegistrationToken) {
    return false;
  }
  if (!file_system_ || !device || !device->Initialize()) {
    return false;
  }

  VirtualFileSystem::RegistrationToken token =
      VirtualFileSystem::kInvalidRegistrationToken;
  if (!file_system_->RegisterDevice(std::move(device), &token)) {
    return false;
  }
  try {
    device_tokens_.push_back(token);
  } catch (...) {
    // Undo the registration if its token can't be recorded.
    file_system_->UnregisterDevice(token);
    return false;
  }
  return true;
}

bool ScopedVfsMount::RegisterSymbolicLink(std::string_view path,
                                          std::string_view target) {
  if (mount_group_token_ != VirtualFileSystem::kInvalidRegistrationToken) {
    return false;
  }
  if (!file_system_) {
    return false;
  }
  VirtualFileSystem::RegistrationToken token =
      VirtualFileSystem::kInvalidRegistrationToken;
  if (!file_system_->RegisterSymbolicLink(path, target, &token)) {
    return false;
  }
  try {
    symbolic_link_tokens_.push_back(token);
  } catch (...) {
    file_system_->UnregisterSymbolicLink(token);
    return false;
  }
  return true;
}

bool ScopedVfsMount::RegisterMountGroup(VirtualFileSystem::MountGroup group) {
  if (!file_system_ || !empty()) {
    return false;
  }
  VirtualFileSystem::RegistrationToken token =
      VirtualFileSystem::kInvalidRegistrationToken;
  if (!file_system_->RegisterMountGroup(std::move(group), &token)) {
    return false;
  }
  mount_group_token_ = token;
  return true;
}

void ScopedVfsMount::Reset() {
  if (!file_system_) {
    return;
  }

  if (mount_group_token_ != VirtualFileSystem::kInvalidRegistrationToken) {
    file_system_->UnregisterMountGroup(mount_group_token_);
    mount_group_token_ = VirtualFileSystem::kInvalidRegistrationToken;
  }

  for (auto it = symbolic_link_tokens_.crbegin();
       it != symbolic_link_tokens_.crend(); ++it) {
    file_system_->UnregisterSymbolicLink(*it);
  }
  symbolic_link_tokens_.clear();

  for (auto it = device_tokens_.crbegin(); it != device_tokens_.crend(); ++it) {
    file_system_->UnregisterDevice(*it);
  }
  device_tokens_.clear();
}

}  // namespace xe::vfs

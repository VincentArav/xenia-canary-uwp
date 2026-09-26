/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
#define XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class VirtualFileSystem {
 public:
  using RegistrationToken = uint64_t;
  static constexpr RegistrationToken kInvalidRegistrationToken = 0;

  struct MountGroupSymbolicLink {
    std::string path;
    std::string target;
  };

  // Devices in a mount group must be initialized before registration. The VFS
  // validates and publishes every device and link under one namespace
  // reservation, or publishes nothing.
  struct MountGroup {
    std::vector<std::unique_ptr<Device>> devices;
    std::vector<MountGroupSymbolicLink> symbolic_links;
  };

  VirtualFileSystem();
  ~VirtualFileSystem();

  void Clear();

  bool RegisterDevice(std::unique_ptr<Device> device,
                      RegistrationToken* registration_token = nullptr);
  bool UnregisterDevice(const std::string_view path);
  bool UnregisterDevice(RegistrationToken registration_token);
  bool IsDeviceRegistered(const std::string_view path);
  // Returns true if a device mounted at path would overlap a registered
  // device's namespace, so that one would shadow the other.
  bool HasDeviceMountPathConflict(const std::string_view path);

  bool RegisterSymbolicLink(const std::string_view path,
                            const std::string_view target,
                            RegistrationToken* registration_token = nullptr);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool UnregisterSymbolicLink(RegistrationToken registration_token);
  bool IsSymbolicLinkRegistered(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  // Atomically reserves and publishes a device/link group. Paths may overlap
  // within the group (a nested device, a file alias inside a group device) but
  // not anything outside it. The returned token owns the group.
  bool RegisterMountGroup(MountGroup group,
                          RegistrationToken* registration_token);
  bool UnregisterMountGroup(RegistrationToken registration_token);

  Entry* ResolvePath(const std::string_view path);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition,
                    uint32_t desired_access, bool is_directory,
                    bool is_non_directory, File** out_file,
                    FileAction* out_action);

  static X_STATUS ExtractContentFile(Entry* entry,
                                     std::filesystem::path base_path,
                                     uint64_t& progress,
                                     bool extract_to_root = false);
  static X_STATUS ExtractContentFiles(Device* device,
                                      std::filesystem::path base_path,
                                      uint64_t& progress);
  static void ExtractContentHeader(Device* device,
                                   std::filesystem::path base_path);

 private:
  enum class SymbolicLinkResolution {
    kUnchanged,
    kResolved,
    kFailed,
  };

  struct RegisteredDevice {
    RegistrationToken token;
    RegistrationToken mount_group_token;
    std::unique_ptr<Device> device;
  };

  struct RegisteredSymbolicLink {
    RegistrationToken token;
    RegistrationToken mount_group_token;
    std::string target;
  };

  struct RegisteredMountGroup {
    RegistrationToken token;
    std::vector<std::string> namespace_paths;
  };

  RegistrationToken AllocateRegistrationTokenLocked();
  bool HasMountGroupPathConflictLocked(std::string_view normalized_path) const;

  xe::global_critical_region global_critical_region_;
  std::vector<RegisteredDevice> devices_;
  std::unordered_map<std::string, RegisteredSymbolicLink> symlinks_;
  std::vector<RegisteredMountGroup> mount_groups_;
  RegistrationToken next_registration_token_ = 1;

  SymbolicLinkResolution ResolveSymbolicLink(const std::string_view path,
                                             std::string& result);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_

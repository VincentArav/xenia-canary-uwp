/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_stfs.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xfile.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// XAPI's XMountUtilityDrive calls these for titles without
// XEX_SYSTEM_INSECURE_UTILITY_DRIVE: it opens \Device\Harddisk0\CacheN, puts
// an STFS volume on it as \Device\cacheN, links \??\cache: to that device
// and later flushes it with StfsControlDevice. Xenia serves cache: from a host
// directory (mount_cache) and ObCreateSymbolicLink keeps an existing link, so
// a well-formed request succeeds without creating a volume.
dword_result_t StfsCreateDevice_entry(
    pointer_t<X_STFS_CREATE_DEVICE_PARAMETERS> parameters,
    dword_t parameters_size) {
  X_STATUS status =
      CheckStfsCreateDeviceParameters(parameters, parameters_size);
  if (XFAILED(status)) {
    XELOGW("StfsCreateDevice: rejected parameter block (size {:X})",
           uint32_t(parameters_size));
    return status;
  }
  auto file = kernel_state()->object_table()->LookupObject<XFile>(
      parameters->backing_file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }
  // There is no device extension to point into.
  if (parameters->extra_extension_size) {
    XELOGW("StfsCreateDevice: {:X} extra extension bytes are not supported",
           uint32_t(parameters->extra_extension_size));
    return X_STATUS_NOT_SUPPORTED;
  }
  const vfs::StfsVolumeDescriptor& descriptor = parameters->volume_descriptor;
  XELOGI(
      "StfsCreateDevice({}) on {} at offset {:X} ({} of {} blocks free{}): "
      "no volume is created, cache: stays on Xenia's cache device",
      util::TranslateAnsiString(kernel_memory(), &parameters->device_name),
      file->path(), uint64_t(parameters->backing_file_offset),
      uint32_t(descriptor.free_block_count),
      uint32_t(descriptor.total_block_count),
      descriptor.flags.bits.read_only_format ? ", read-only format" : "");
  // StfsControlDevice accepts this null device object.
  parameters->device_object = 0;
  parameters->extra_extension = 0;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(StfsCreateDevice, kFileSystem, kStub);

// StfsCreateDevice creates no volume, so whatever device object is passed has
// nothing behind it (see ControlHostCacheStfsDevice).
dword_result_t StfsControlDevice_entry(dword_t device_object,
                                       dword_t control_code, lpvoid_t buffer) {
  const X_STATUS status =
      ControlHostCacheStfsDevice(control_code, buffer.as<uint8_t*>());
  if (XFAILED(status)) {
    XELOGW("StfsControlDevice({:08X}, {:X}): unknown control code",
           uint32_t(device_object), uint32_t(control_code));
  } else {
    XELOGD("StfsControlDevice({:08X}, {}): nothing to do",
           uint32_t(device_object), StfsControlCodeName(control_code));
  }
  return status;
}
DECLARE_XBOXKRNL_EXPORT1(StfsControlDevice, kFileSystem, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Stfs);

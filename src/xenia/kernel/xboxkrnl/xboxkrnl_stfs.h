/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_STFS_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_STFS_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/kernel.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// StfsCreateDevice(parameters, parameters_size) puts an STFS volume that
// lives in an open file behind a new device object, and
// StfsControlDevice(device_object, control_code, buffer) controls it. This is
// the parameter block XAPI's XMountUtilityDrive passes.
struct X_STFS_CREATE_DEVICE_PARAMETERS {
  // Name of the device object to create, such as \Device\cache0.
  X_ANSI_STRING device_name;  // 0x00
  // Byte offset of the volume's block 0 in the backing file.
  xe::be<uint64_t> backing_file_offset;  // 0x08
  // Bytes of the backing file the volume may use (writable format only).
  xe::be<uint64_t> backing_file_capacity;       // 0x10
  xe::be<uint32_t> backing_file_handle;         // 0x18
  vfs::StfsVolumeDescriptor volume_descriptor;  // 0x1C
  // Caller-allocated block cache, block_cache_element_count 4 KB blocks.
  xe::be<uint32_t> block_cache;  // 0x40
  // Bytes the caller wants in the device extension after the driver's own.
  xe::be<uint32_t> extra_extension_size;  // 0x44
  // Out, on success: the device object and the caller's extension bytes.
  xe::be<uint32_t> device_object;     // 0x48
  xe::be<uint32_t> extra_extension;   // 0x4C
  uint8_t block_cache_element_count;  // 0x50
  // Nonzero: IoCreateDevice gets 0x1000 instead of 0x2000 as the argument
  // Xenia calls extra_device_object_attributes.
  uint8_t unknown_51;  // 0x51
  // Nonzero: the backing file already spans backing_file_capacity, so it is
  // never extended and the volume reports the capacity as its size.
  uint8_t preallocated;  // 0x52
  // Reported as the characteristics in FileFsDeviceInformation.
  uint8_t device_characteristics;  // 0x53
  uint8_t pad_54[4];               // 0x54
};
static_assert_size(X_STFS_CREATE_DEVICE_PARAMETERS, 0x58);
static_assert(offsetof(X_STFS_CREATE_DEVICE_PARAMETERS, backing_file_handle) ==
              0x18);
static_assert(offsetof(X_STFS_CREATE_DEVICE_PARAMETERS, volume_descriptor) ==
              0x1C);
static_assert(offsetof(X_STFS_CREATE_DEVICE_PARAMETERS, block_cache) == 0x40);
static_assert(offsetof(X_STFS_CREATE_DEVICE_PARAMETERS,
                       block_cache_element_count) == 0x50);

// The driver needs at least this many block cache elements.
constexpr uint8_t kStfsMinimumBlockCacheElementCount = 5;

// The status the driver returns for a parameter block it rejects before it
// looks at the backing file: a parameters_size other than 0x58 or fewer than
// five block cache elements (a null block is rejected the same way here; the
// driver would fault). X_STATUS_SUCCESS if the block passes.
inline X_STATUS CheckStfsCreateDeviceParameters(
    const X_STFS_CREATE_DEVICE_PARAMETERS* parameters,
    uint32_t parameters_size) {
  if (parameters_size != sizeof(X_STFS_CREATE_DEVICE_PARAMETERS) ||
      !parameters ||
      parameters->block_cache_element_count <
          kStfsMinimumBlockCacheElementCount) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return X_STATUS_SUCCESS;
}

// StfsControlDevice control codes. Every code but the two lock codes first
// returns the volume's recorded failure, if it has one.
enum StfsControlCode : uint32_t {
  // Enter and leave the volume's critical section.
  kStfsControlLockVolume = 0,
  kStfsControlUnlockVolume = 1,
  // Write dirty directory entries, then the dirty cache blocks.
  kStfsControlFlush = 2,
  // Fill the buffer with the volume's current descriptor (0x24 bytes).
  kStfsControlQueryVolumeDescriptor = 3,
  // Blocks freed since the last commit become allocatable and the per-commit
  // flags of open files and cached blocks are cleared.
  kStfsControlCommit = 4,
  // Read or write a 32-bit allocation setting (0x14 when created).
  kStfsControlGetAllocationSetting = 5,
  kStfsControlSetAllocationSetting = 6,
  // Copy 0x28 bytes of driver-wide statistics (read and write counts) out.
  kStfsControlQueryStatistics = 7,
  // Buffer holds a routine and its context, called once when the volume
  // first records a failure.
  kStfsControlSetFailureCallback = 8,
};
// Codes above this fail with X_STATUS_INVALID_DEVICE_REQUEST.
constexpr uint32_t kStfsControlMaximum = kStfsControlSetFailureCallback;

// The allocation setting a new volume starts with.
constexpr uint32_t kStfsInitialAllocationSetting = 0x14;
// Bytes kStfsControlQueryStatistics copies out.
constexpr size_t kStfsStatisticsSize = 0x28;

// StfsControlDevice for the device StfsCreateDevice hands out, which has no
// volume behind it: locking, flushing and committing do nothing and settings
// are accepted. A non-null buffer gets the initial allocation setting (code 5)
// or zeroed statistics (code 7); the descriptor query (code 3) leaves the
// buffer as it is. Other codes fail as in the driver.
inline X_STATUS ControlHostCacheStfsDevice(uint32_t control_code,
                                           uint8_t* buffer) {
  switch (control_code) {
    case kStfsControlGetAllocationSetting:
      if (buffer) {
        xe::store_and_swap<uint32_t>(buffer,
                                     uint32_t(kStfsInitialAllocationSetting));
      }
      return X_STATUS_SUCCESS;
    case kStfsControlQueryStatistics:
      if (buffer) {
        std::memset(buffer, 0, kStfsStatisticsSize);
      }
      return X_STATUS_SUCCESS;
    case kStfsControlLockVolume:
    case kStfsControlUnlockVolume:
    case kStfsControlFlush:
    case kStfsControlQueryVolumeDescriptor:
    case kStfsControlCommit:
    case kStfsControlSetAllocationSetting:
    case kStfsControlSetFailureCallback:
      return X_STATUS_SUCCESS;
    default:
      return X_STATUS_INVALID_DEVICE_REQUEST;
  }
}

inline const char* StfsControlCodeName(uint32_t control_code) {
  switch (control_code) {
    case kStfsControlLockVolume:
      return "lock volume";
    case kStfsControlUnlockVolume:
      return "unlock volume";
    case kStfsControlFlush:
      return "flush";
    case kStfsControlQueryVolumeDescriptor:
      return "query volume descriptor";
    case kStfsControlCommit:
      return "commit";
    case kStfsControlGetAllocationSetting:
      return "get allocation setting";
    case kStfsControlSetAllocationSetting:
      return "set allocation setting";
    case kStfsControlQueryStatistics:
      return "query statistics";
    case kStfsControlSetFailureCallback:
      return "set failure callback";
    default:
      return "unknown";
  }
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_STFS_H_

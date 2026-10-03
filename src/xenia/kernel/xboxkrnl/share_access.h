/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_SHARE_ACCESS_H_
#define XENIA_KERNEL_XBOXKRNL_SHARE_ACCESS_H_

#include <cstdint>

// NT share-access bookkeeping (IoCheckShareAccess, IoSetShareAccess,
// IoRemoveShareAccess) over the Xbox 360 structures. SHARE_ACCESS is made of
// byte counters. The access an open was granted is kept in FILE_OBJECT byte 3
// (byte 2 holds the FO_* flags), its bits allocated from the most significant:
// DeletePending, ReadAccess, WriteAccess, DeleteAccess, SharedRead,
// SharedWrite, SharedDelete.

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace share_access {

struct X_SHARE_ACCESS {
  uint8_t open_count;     // 0x0
  uint8_t readers;        // 0x1
  uint8_t writers;        // 0x2
  uint8_t deleters;       // 0x3
  uint8_t shared_read;    // 0x4
  uint8_t shared_write;   // 0x5
  uint8_t shared_delete;  // 0x6
};
static_assert(sizeof(X_SHARE_ACCESS) == 7, "SHARE_ACCESS is seven bytes");

// Offset of the access bit-field byte within the guest FILE_OBJECT.
constexpr uint32_t kFileObjectAccessByteOffset = 3;

// Bits of that byte.
constexpr uint8_t kFileObjectReadAccess = 0x40;
constexpr uint8_t kFileObjectWriteAccess = 0x20;
constexpr uint8_t kFileObjectDeleteAccess = 0x10;
constexpr uint8_t kFileObjectSharedRead = 0x08;
constexpr uint8_t kFileObjectSharedWrite = 0x04;
constexpr uint8_t kFileObjectSharedDelete = 0x02;
constexpr uint8_t kFileObjectAccessMask =
    kFileObjectReadAccess | kFileObjectWriteAccess | kFileObjectDeleteAccess;
constexpr uint8_t kFileObjectSharedMask =
    kFileObjectSharedRead | kFileObjectSharedWrite | kFileObjectSharedDelete;

// ACCESS_MASK and share-mode bits that take part in the check. Reading and
// writing attributes or extended attributes do not.
constexpr uint32_t kFileReadData = 0x00000001;
constexpr uint32_t kFileWriteData = 0x00000002;
constexpr uint32_t kFileAppendData = 0x00000004;
constexpr uint32_t kFileExecute = 0x00000020;
constexpr uint32_t kDelete = 0x00010000;
constexpr uint32_t kFileShareRead = 0x00000001;
constexpr uint32_t kFileShareWrite = 0x00000002;
constexpr uint32_t kFileShareDelete = 0x00000004;

constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusSharingViolation = 0xC0000043;

// Records the access an open asks for in the file object's access byte, as
// the start of IoCheckShareAccess and IoSetShareAccess does. The share bits
// are only rewritten when the open asks for read, write or delete access;
// otherwise the open takes no part in sharing and they keep their value.
// DeletePending and the reserved bit are preserved.
inline uint8_t RecordAccess(uint8_t file_object_bits, uint32_t desired_access,
                            uint32_t desired_share_access) {
  uint8_t bits =
      static_cast<uint8_t>(file_object_bits & ~kFileObjectAccessMask);
  if (desired_access & (kFileExecute | kFileReadData)) {
    bits |= kFileObjectReadAccess;
  }
  if (desired_access & (kFileWriteData | kFileAppendData)) {
    bits |= kFileObjectWriteAccess;
  }
  if (desired_access & kDelete) {
    bits |= kFileObjectDeleteAccess;
  }
  if (bits & kFileObjectAccessMask) {
    bits = static_cast<uint8_t>(bits & ~kFileObjectSharedMask);
    if (desired_share_access & kFileShareRead) {
      bits |= kFileObjectSharedRead;
    }
    if (desired_share_access & kFileShareWrite) {
      bits |= kFileObjectSharedWrite;
    }
    if (desired_share_access & kFileShareDelete) {
      bits |= kFileObjectSharedDelete;
    }
  }
  return bits;
}

// Adds (delta 1) or removes (delta -1) one open with the recorded access bits
// to or from every counter it takes part in.
inline void CountOpen(uint8_t file_object_bits, X_SHARE_ACCESS& share,
                      int delta) {
  auto adjust = [delta](uint8_t& counter, bool counted) {
    if (counted) {
      counter = static_cast<uint8_t>(counter + delta);
    }
  };
  adjust(share.open_count, true);
  adjust(share.readers, (file_object_bits & kFileObjectReadAccess) != 0);
  adjust(share.writers, (file_object_bits & kFileObjectWriteAccess) != 0);
  adjust(share.deleters, (file_object_bits & kFileObjectDeleteAccess) != 0);
  adjust(share.shared_read, (file_object_bits & kFileObjectSharedRead) != 0);
  adjust(share.shared_write, (file_object_bits & kFileObjectSharedWrite) != 0);
  adjust(share.shared_delete,
         (file_object_bits & kFileObjectSharedDelete) != 0);
}

// IoCheckShareAccess. Returns STATUS_SHARING_VIOLATION when the open conflicts
// with how the file is already open, otherwise STATUS_SUCCESS, counting the
// open into share when update is set. The counters are bytes, as on the
// console, and wrap the same way.
inline uint32_t CheckShareAccess(uint32_t desired_access,
                                 uint32_t desired_share_access,
                                 uint8_t& file_object_bits,
                                 X_SHARE_ACCESS& share, bool update) {
  file_object_bits =
      RecordAccess(file_object_bits, desired_access, desired_share_access);
  const uint8_t bits = file_object_bits;
  if (!(bits & kFileObjectAccessMask)) {
    return kStatusSuccess;
  }
  const uint8_t open_count = share.open_count;
  if (((bits & kFileObjectReadAccess) && share.shared_read < open_count) ||
      ((bits & kFileObjectWriteAccess) && share.shared_write < open_count) ||
      ((bits & kFileObjectDeleteAccess) && share.shared_delete < open_count) ||
      (share.readers && !(bits & kFileObjectSharedRead)) ||
      (share.writers && !(bits & kFileObjectSharedWrite)) ||
      (share.deleters && !(bits & kFileObjectSharedDelete))) {
    return kStatusSharingViolation;
  }
  if (update) {
    CountOpen(bits, share, 1);
  }
  return kStatusSuccess;
}

// IoSetShareAccess: the first open of a file. share starts over from this
// open alone, or is cleared when the open asks for none of read, write and
// delete access.
inline void SetShareAccess(uint32_t desired_access,
                           uint32_t desired_share_access,
                           uint8_t& file_object_bits, X_SHARE_ACCESS& share) {
  file_object_bits =
      RecordAccess(file_object_bits, desired_access, desired_share_access);
  share = X_SHARE_ACCESS{};
  if (file_object_bits & kFileObjectAccessMask) {
    CountOpen(file_object_bits, share, 1);
  }
}

// IoRemoveShareAccess: takes the open recorded in file_object_bits back out
// of share. Opens without read, write or delete access were never counted.
inline void RemoveShareAccess(uint8_t file_object_bits, X_SHARE_ACCESS& share) {
  const uint8_t bits = file_object_bits;
  if (!(bits & kFileObjectAccessMask)) {
    return;
  }
  CountOpen(bits, share, -1);
}

}  // namespace share_access
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_SHARE_ACCESS_H_

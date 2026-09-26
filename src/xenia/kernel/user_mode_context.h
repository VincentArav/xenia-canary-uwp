/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_CONTEXT_H_
#define XENIA_KERNEL_USER_MODE_CONTEXT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe::kernel {

inline constexpr size_t kUserModeContextMinimumSize = 0x22C;
inline constexpr size_t kUserModeContextEntryAddressOffset = 0x008;
inline constexpr size_t kUserModeContextLinkRegisterOffset = 0x00C;
inline constexpr size_t kUserModeContextCountRegisterOffset = 0x010;
inline constexpr size_t kUserModeContextGprOffset = 0x018;
inline constexpr size_t kUserModeContextConditionRegisterOffset = 0x118;
inline constexpr size_t kUserModeContextXerOffset = 0x11C;
inline constexpr size_t kUserModeContextControlOffset = 0x228;

// The trap frame passed (r4) to a KeEnterUserMode handler for a system call
// or exception. Only these fields are written and read back; the instruction
// address is where the thread resumes.
inline constexpr size_t kUserModeTrapFrameSize = 0x1C0;
inline constexpr size_t kUserModeTrapFrameGprOffset = 0x050;
inline constexpr size_t kUserModeTrapFrameCountRegisterOffset = 0x1A0;
inline constexpr size_t kUserModeTrapFrameLinkRegisterOffset = 0x1A8;
inline constexpr size_t kUserModeTrapFrameInstructionAddressOffset = 0x1AC;
inline constexpr size_t kUserModeTrapFrameConditionRegisterOffset = 0x1B0;
inline constexpr size_t kUserModeTrapFrameXerOffset = 0x1B8;

enum class UserModeContextCodecError {
  kNone,
  kBufferTooSmall,
};

// The integer and control fields of a user-mode context; other bytes are left
// as they are.
struct UserModeIntegerContext {
  uint32_t entry_address = 0;
  uint32_t link_register = 0;
  uint64_t count_register = 0;
  std::array<uint64_t, 32> gpr{};
  uint32_t condition_register = 0;
  uint32_t xer = 0;
  uint32_t user_mode_control = 0;
};

struct UserModeIntegerContextResult {
  UserModeContextCodecError error = UserModeContextCodecError::kNone;
  UserModeIntegerContext context;

  explicit operator bool() const {
    return error == UserModeContextCodecError::kNone;
  }
};

// Decodes the verified big-endian fields from a buffer of at least 0x22C
// bytes. Larger buffers are accepted.
UserModeIntegerContextResult DecodeUserModeIntegerContext(
    std::span<const uint8_t> bytes);

// Encodes only the verified fields, preserving every uninterpreted byte in
// the destination buffer.
UserModeContextCodecError EncodeUserModeIntegerContext(
    const UserModeIntegerContext& context, std::span<uint8_t> bytes);

// Trap-frame codec. entry_address maps to the frame's instruction address;
// user_mode_control has no frame field and is neither written nor changed.
UserModeContextCodecError EncodeUserModeTrapFrame(
    const UserModeIntegerContext& context, std::span<uint8_t> bytes);
UserModeContextCodecError DecodeUserModeTrapFrame(
    std::span<const uint8_t> bytes, UserModeIntegerContext& context);

// Saves the integer registers; LR is truncated to 32 bits, and the entry
// address and UserModeControl are kept. XER holds only SO, OV and CA.
void SaveUserModeIntegerContext(const cpu::ppc::PPCContext& source,
                                UserModeIntegerContext& destination);

// Applies the integer registers; other state is untouched.
void ApplyUserModeIntegerContext(const UserModeIntegerContext& source,
                                 cpu::ppc::PPCContext& destination);

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_CONTEXT_H_

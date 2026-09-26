/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_context.h"

#include <algorithm>
#include <iterator>

namespace xe::kernel {
namespace {

uint32_t ReadBigEndian32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
         uint32_t(bytes[offset + 2]) << 8 | uint32_t(bytes[offset + 3]);
}

uint64_t ReadBigEndian64(std::span<const uint8_t> bytes, size_t offset) {
  return uint64_t(ReadBigEndian32(bytes, offset)) << 32 |
         ReadBigEndian32(bytes, offset + 4);
}

void WriteBigEndian32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  bytes[offset] = uint8_t(value >> 24);
  bytes[offset + 1] = uint8_t(value >> 16);
  bytes[offset + 2] = uint8_t(value >> 8);
  bytes[offset + 3] = uint8_t(value);
}

void WriteBigEndian64(std::span<uint8_t> bytes, size_t offset, uint64_t value) {
  WriteBigEndian32(bytes, offset, uint32_t(value >> 32));
  WriteBigEndian32(bytes, offset + 4, uint32_t(value));
}

uint32_t CaptureXer(const cpu::ppc::PPCContext& context) {
  return uint32_t(context.xer_so & 1) << 31 |
         uint32_t(context.xer_ov & 1) << 30 |
         uint32_t(context.xer_ca & 1) << 29;
}

void ApplyXer(uint32_t xer, cpu::ppc::PPCContext& context) {
  context.xer_so = uint8_t((xer >> 31) & 1);
  context.xer_ov = uint8_t((xer >> 30) & 1);
  context.xer_ca = uint8_t((xer >> 29) & 1);
}

}  // namespace

UserModeIntegerContextResult DecodeUserModeIntegerContext(
    std::span<const uint8_t> bytes) {
  UserModeIntegerContextResult result;
  if (bytes.size() < kUserModeContextMinimumSize) {
    result.error = UserModeContextCodecError::kBufferTooSmall;
    return result;
  }

  result.context.entry_address =
      ReadBigEndian32(bytes, kUserModeContextEntryAddressOffset);
  result.context.link_register =
      ReadBigEndian32(bytes, kUserModeContextLinkRegisterOffset);
  result.context.count_register =
      ReadBigEndian64(bytes, kUserModeContextCountRegisterOffset);
  for (size_t i = 0; i < result.context.gpr.size(); ++i) {
    result.context.gpr[i] =
        ReadBigEndian64(bytes, kUserModeContextGprOffset + i * 8);
  }
  result.context.condition_register =
      ReadBigEndian32(bytes, kUserModeContextConditionRegisterOffset);
  result.context.xer = ReadBigEndian32(bytes, kUserModeContextXerOffset);
  result.context.user_mode_control =
      ReadBigEndian32(bytes, kUserModeContextControlOffset);
  return result;
}

UserModeContextCodecError EncodeUserModeIntegerContext(
    const UserModeIntegerContext& context, std::span<uint8_t> bytes) {
  if (bytes.size() < kUserModeContextMinimumSize) {
    return UserModeContextCodecError::kBufferTooSmall;
  }

  WriteBigEndian32(bytes, kUserModeContextEntryAddressOffset,
                   context.entry_address);
  WriteBigEndian32(bytes, kUserModeContextLinkRegisterOffset,
                   context.link_register);
  WriteBigEndian64(bytes, kUserModeContextCountRegisterOffset,
                   context.count_register);
  for (size_t i = 0; i < context.gpr.size(); ++i) {
    WriteBigEndian64(bytes, kUserModeContextGprOffset + i * 8, context.gpr[i]);
  }
  WriteBigEndian32(bytes, kUserModeContextConditionRegisterOffset,
                   context.condition_register);
  WriteBigEndian32(bytes, kUserModeContextXerOffset, context.xer);
  WriteBigEndian32(bytes, kUserModeContextControlOffset,
                   context.user_mode_control);
  return UserModeContextCodecError::kNone;
}

UserModeContextCodecError EncodeUserModeTrapFrame(
    const UserModeIntegerContext& context, std::span<uint8_t> bytes) {
  if (bytes.size() < kUserModeTrapFrameSize) {
    return UserModeContextCodecError::kBufferTooSmall;
  }
  for (size_t i = 0; i < context.gpr.size(); ++i) {
    WriteBigEndian64(bytes, kUserModeTrapFrameGprOffset + i * 8,
                     context.gpr[i]);
  }
  WriteBigEndian64(bytes, kUserModeTrapFrameCountRegisterOffset,
                   context.count_register);
  WriteBigEndian32(bytes, kUserModeTrapFrameLinkRegisterOffset,
                   context.link_register);
  WriteBigEndian32(bytes, kUserModeTrapFrameInstructionAddressOffset,
                   context.entry_address);
  WriteBigEndian32(bytes, kUserModeTrapFrameConditionRegisterOffset,
                   context.condition_register);
  WriteBigEndian32(bytes, kUserModeTrapFrameXerOffset, context.xer);
  return UserModeContextCodecError::kNone;
}

UserModeContextCodecError DecodeUserModeTrapFrame(
    std::span<const uint8_t> bytes, UserModeIntegerContext& context) {
  if (bytes.size() < kUserModeTrapFrameSize) {
    return UserModeContextCodecError::kBufferTooSmall;
  }
  for (size_t i = 0; i < context.gpr.size(); ++i) {
    context.gpr[i] =
        ReadBigEndian64(bytes, kUserModeTrapFrameGprOffset + i * 8);
  }
  context.count_register =
      ReadBigEndian64(bytes, kUserModeTrapFrameCountRegisterOffset);
  context.link_register =
      ReadBigEndian32(bytes, kUserModeTrapFrameLinkRegisterOffset);
  context.entry_address =
      ReadBigEndian32(bytes, kUserModeTrapFrameInstructionAddressOffset);
  context.condition_register =
      ReadBigEndian32(bytes, kUserModeTrapFrameConditionRegisterOffset);
  context.xer = ReadBigEndian32(bytes, kUserModeTrapFrameXerOffset);
  return UserModeContextCodecError::kNone;
}

void SaveUserModeIntegerContext(const cpu::ppc::PPCContext& source,
                                UserModeIntegerContext& destination) {
  std::copy(std::begin(source.r), std::end(source.r), destination.gpr.begin());
  destination.link_register = uint32_t(source.lr);
  destination.count_register = source.ctr;
  destination.condition_register = uint32_t(source.cr());
  destination.xer = CaptureXer(source);
}

void ApplyUserModeIntegerContext(const UserModeIntegerContext& source,
                                 cpu::ppc::PPCContext& destination) {
  std::copy(source.gpr.begin(), source.gpr.end(), std::begin(destination.r));
  destination.lr = source.link_register;
  destination.ctr = source.count_register;
  destination.set_cr(source.condition_register);
  ApplyXer(source.xer, destination);
}

}  // namespace xe::kernel

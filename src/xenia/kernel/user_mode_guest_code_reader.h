/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_GUEST_CODE_READER_H_
#define XENIA_KERNEL_USER_MODE_GUEST_CODE_READER_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "xenia/cpu/ppc/guest_code_reader.h"
#include "xenia/kernel/user_mode_address_space.h"
#include "xenia/kernel/user_mode_manager.h"
#include "xenia/kernel/user_mode_physical_memory.h"

namespace xe::kernel {

enum class UserModeGuestCodeReadDiagnostic {
  kNone,
  kInvalidConfiguration,
  kInvalidInstructionAddress,
  kMappingNotCurrent,
  kAddressTranslationFailed,
  kInstructionAccessFault,
  kPhysicalReadFailed,
};

enum class UserModeGuestCodePhysicalReadStage {
  kNone,
  kRootEntry,
  kPageTableEntry,
  kInstruction,
};

struct UserModeGuestCodeReadResult {
  UserModeGuestCodeReadDiagnostic diagnostic =
      UserModeGuestCodeReadDiagnostic::kNone;
  uint32_t requested_instruction_address = 0;
  uint32_t storage_effective_address = 0;
  std::optional<UserModeAddressTranslationResult> address_translation;
  std::optional<UserModePhysicalReadResult> physical_read_failure;
  UserModeGuestCodePhysicalReadStage physical_read_failure_stage =
      UserModeGuestCodePhysicalReadStage::kNone;
  std::optional<uint32_t> instruction;

  explicit operator bool() const {
    return diagnostic == UserModeGuestCodeReadDiagnostic::kNone &&
           instruction.has_value();
  }
};

// Classifies a failed fetch as an instruction storage fault. Failures that are
// not the guest's (bad configuration, a misaligned address, a retired mapping)
// report no guest fault.
cpu::ppc::GuestInstructionFetchFault ClassifyUserModeInstructionFetchFailure(
    const UserModeGuestCodeReadResult& result);

// Fetches instructions through one user-mode mapping view. Borrows Memory,
// which must outlive the descriptors that own the reader.
class UserModeGuestCodeReader final : public cpu::ppc::GuestCodeReader {
 public:
  UserModeGuestCodeReader(
      Memory* memory,
      std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
      UserModeProblemStateKey problem_state_key, bool little_endian);

  UserModeGuestCodeReadResult ReadInstruction(uint32_t guest_address) const;

  cpu::MemoryAccessMode memory_access_mode() const override {
    return cpu::MemoryAccessMode::kChecked;
  }
  bool TryReadInstruction(uint32_t guest_address,
                          uint32_t* instruction_out) const override;
  bool TryReadInstructionWithFault(
      uint32_t guest_address, uint32_t* instruction_out,
      uint32_t* source_address_out,
      cpu::ppc::GuestInstructionFetchFault* fault_out) const override;
  bool TryReadInstructionWithSource(
      uint32_t guest_address, uint32_t* instruction_out,
      uint32_t* source_address_out) const override;

  UserModeMappingHandle mapping_handle() const {
    return generation_token_ ? generation_token_->handle()
                             : UserModeMappingHandle{};
  }
  UserModeProblemStateKey problem_state_key() const {
    return problem_state_key_;
  }
  bool little_endian() const { return little_endian_; }

 private:
  bool HasValidConfiguration() const;
  bool IsMappingCurrent() const;

  Memory* const memory_;
  const std::shared_ptr<const UserModeMappingGenerationToken> generation_token_;
  const UserModeProblemStateKey problem_state_key_;
  const bool little_endian_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_GUEST_CODE_READER_H_

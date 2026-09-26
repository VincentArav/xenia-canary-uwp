/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_guest_code_reader.h"

#include <array>
#include <span>
#include <utility>

#include "xenia/kernel/user_mode_physical_memory.h"

namespace xe::kernel {

UserModeGuestCodeReader::UserModeGuestCodeReader(
    Memory* memory,
    std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
    UserModeProblemStateKey problem_state_key, bool little_endian)
    : memory_(memory),
      generation_token_(std::move(generation_token)),
      problem_state_key_(problem_state_key),
      little_endian_(little_endian) {}

bool UserModeGuestCodeReader::HasValidConfiguration() const {
  if (!memory_ || !generation_token_ ||
      generation_token_->control_page_physical_address() == 0 ||
      (generation_token_->control_page_physical_address() & 0xFFF) != 0 ||
      generation_token_->control_page_physical_address() >= 0x20000000 ||
      !generation_token_->create_time_layout().has_value()) {
    return false;
  }
  switch (problem_state_key_) {
    case UserModeProblemStateKey::kZero:
    case UserModeProblemStateKey::kOne:
      return static_cast<bool>(generation_token_->handle());
    default:
      return false;
  }
}

bool UserModeGuestCodeReader::IsMappingCurrent() const {
  return generation_token_ && generation_token_->is_current();
}

UserModeGuestCodeReadResult UserModeGuestCodeReader::ReadInstruction(
    uint32_t guest_address) const {
  UserModeGuestCodeReadResult result;
  result.requested_instruction_address = guest_address;
  if (!HasValidConfiguration()) {
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kInvalidConfiguration;
    return result;
  }
  if (guest_address & (sizeof(uint32_t) - 1)) {
    result.diagnostic =
        UserModeGuestCodeReadDiagnostic::kInvalidInstructionAddress;
    return result;
  }
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kMappingNotCurrent;
    return result;
  }

  // Little-endian mode munges the address (EA[2] inverted for a word) rather
  // than reversing bytes; instructions stay big-endian.
  const uint32_t storage_effective_address =
      guest_address ^ (little_endian_ ? uint32_t{4} : uint32_t{0});
  result.storage_effective_address = storage_effective_address;

  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kMappingNotCurrent;
    return result;
  }

  size_t translation_read_index = 0;
  const UserModeCreateTimeAddressSpaceLayout& create_time_layout =
      *generation_token_->create_time_layout();
  const UserModePageTableKind page_table_kind =
      create_time_layout
          .segment_for_effective_address(storage_effective_address)
          .translation.page_table_kind;
  result.address_translation = ResolveUserModeLiveAddress(
      create_time_layout, generation_token_->control_page_physical_address(),
      problem_state_key_, UserModeMemoryAccess::kInstructionFetch,
      storage_effective_address,
      [&transaction, &result, &translation_read_index, page_table_kind](
          uint32_t physical_address, std::span<uint8_t> destination) {
        const bool has_root_entry =
            page_table_kind != UserModePageTableKind::k16MiB;
        const UserModeGuestCodePhysicalReadStage stage =
            has_root_entry && translation_read_index == 0
                ? UserModeGuestCodePhysicalReadStage::kRootEntry
                : UserModeGuestCodePhysicalReadStage::kPageTableEntry;
        ++translation_read_index;
        const UserModePhysicalReadResult read =
            transaction.TryRead(physical_address, destination);
        if (!read) {
          result.physical_read_failure = read;
          result.physical_read_failure_stage = stage;
        }
        return static_cast<bool>(read);
      });

  // A retired mapping takes precedence over other failures.
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kMappingNotCurrent;
    return result;
  }
  if (!result.address_translation->translated()) {
    result.diagnostic =
        UserModeGuestCodeReadDiagnostic::kAddressTranslationFailed;
    return result;
  }
  if (!result.address_translation->access_permitted()) {
    result.diagnostic =
        UserModeGuestCodeReadDiagnostic::kInstructionAccessFault;
    return result;
  }

  std::array<uint8_t, sizeof(uint32_t)> instruction_bytes{};
  const UserModePhysicalReadResult physical_read = transaction.TryRead(
      result.address_translation->physical_address, instruction_bytes);
  if (!physical_read) {
    if (!IsMappingCurrent()) {
      result.diagnostic = UserModeGuestCodeReadDiagnostic::kMappingNotCurrent;
      return result;
    }
    result.physical_read_failure = physical_read;
    result.physical_read_failure_stage =
        UserModeGuestCodePhysicalReadStage::kInstruction;
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kPhysicalReadFailed;
    return result;
  }

  const uint32_t decoded_instruction =
      uint32_t(uint32_t(instruction_bytes[0]) << 24 |
               uint32_t(instruction_bytes[1]) << 16 |
               uint32_t(instruction_bytes[2]) << 8 | instruction_bytes[3]);
  // Check retirement again before returning the word.
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestCodeReadDiagnostic::kMappingNotCurrent;
    return result;
  }
  result.instruction = decoded_instruction;
  return result;
}

bool UserModeGuestCodeReader::TryReadInstruction(
    uint32_t guest_address, uint32_t* instruction_out) const {
  uint32_t source_address;
  return TryReadInstructionWithSource(guest_address, instruction_out,
                                      &source_address);
}

cpu::ppc::GuestInstructionFetchFault ClassifyUserModeInstructionFetchFailure(
    const UserModeGuestCodeReadResult& result) {
  using Reason = cpu::ppc::GuestInstructionFetchFaultReason;
  cpu::ppc::GuestInstructionFetchFault fault;
  fault.instruction_address = result.requested_instruction_address;
  fault.storage_effective_address = result.storage_effective_address;
  fault.backend_diagnostic = static_cast<uint32_t>(result.diagnostic);
  switch (result.diagnostic) {
    case UserModeGuestCodeReadDiagnostic::kAddressTranslationFailed:
      if (result.address_translation) {
        switch (result.address_translation->diagnostic) {
          case UserModeAddressTranslationDiagnostic::kRootNotPresent:
          case UserModeAddressTranslationDiagnostic::kPageNotPresent:
            fault.reason = Reason::kAddressTranslation;
            break;
          case UserModeAddressTranslationDiagnostic::kBackingReadFailed:
          case UserModeAddressTranslationDiagnostic::kBackingAddressOverflow:
            fault.reason = Reason::kPhysicalBacking;
            break;
          default:
            break;
        }
      }
      break;
    case UserModeGuestCodeReadDiagnostic::kInstructionAccessFault: {
      const uint32_t faults =
          result.address_translation
              ? static_cast<uint32_t>(result.address_translation->access_faults)
              : 0;
      if (faults &
          static_cast<uint32_t>(UserModeAddressAccessFault::kNoExecute)) {
        fault.reason = Reason::kNoExecute;
      } else if (faults &
                 static_cast<uint32_t>(
                     UserModeAddressAccessFault::kGuardedInstruction)) {
        fault.reason = Reason::kGuarded;
      } else {
        fault.reason = Reason::kProtection;
      }
      break;
    }
    case UserModeGuestCodeReadDiagnostic::kPhysicalReadFailed:
      fault.reason = Reason::kPhysicalBacking;
      break;
    case UserModeGuestCodeReadDiagnostic::kMappingNotCurrent:
      fault.reason = Reason::kMappingRetired;
      break;
    default:
      break;
  }
  return fault;
}

bool UserModeGuestCodeReader::TryReadInstructionWithFault(
    uint32_t guest_address, uint32_t* instruction_out,
    uint32_t* source_address_out,
    cpu::ppc::GuestInstructionFetchFault* fault_out) const {
  if (!instruction_out || !source_address_out || !fault_out) {
    return false;
  }
  *fault_out = {};
  fault_out->instruction_address = guest_address;
  const UserModeGuestCodeReadResult result = ReadInstruction(guest_address);
  if (!result) {
    *fault_out = ClassifyUserModeInstructionFetchFailure(result);
    return false;
  }
  // And once more before writing the output.
  if (!IsMappingCurrent()) {
    fault_out->reason =
        cpu::ppc::GuestInstructionFetchFaultReason::kMappingRetired;
    return false;
  }
  *instruction_out = *result.instruction;
  // The storage word actually fetched, after little-endian munging.
  *source_address_out = result.address_translation->physical_address;
  return true;
}

bool UserModeGuestCodeReader::TryReadInstructionWithSource(
    uint32_t guest_address, uint32_t* instruction_out,
    uint32_t* source_address_out) const {
  cpu::ppc::GuestInstructionFetchFault fault;
  return TryReadInstructionWithFault(guest_address, instruction_out,
                                     source_address_out, &fault);
}

}  // namespace xe::kernel

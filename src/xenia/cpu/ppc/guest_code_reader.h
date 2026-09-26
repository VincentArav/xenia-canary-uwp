/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_GUEST_CODE_READER_H_
#define XENIA_CPU_PPC_GUEST_CODE_READER_H_

#include <cstdint>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/translation_domain.h"
#include "xenia/memory.h"

namespace xe::cpu::ppc {

// Source address reported by readers that don't know where a word came from.
inline constexpr uint32_t kUnknownInstructionSource = UINT32_MAX;

enum class GuestInstructionFetchFaultReason : uint8_t {
  // Fetched, or failed for a host or configuration reason: not a guest fault.
  kNone,
  // A segment root or page table entry is not present.
  kAddressTranslation,
  // The page protection denies the fetch for the current key.
  kProtection,
  // The segment or the page is no-execute.
  kNoExecute,
  // The page is guarded.
  kGuarded,
  // The page tables or the word could not be read from physical memory.
  kPhysicalBacking,
  // The mapping was retired while reading: not a guest fault.
  kMappingRetired,
};

// Why an instruction fetch failed, in the terms of an instruction storage
// interrupt. Trivially destructible: it is live across nonlocal exits.
struct GuestInstructionFetchFault {
  GuestInstructionFetchFaultReason reason =
      GuestInstructionFetchFaultReason::kNone;
  // SRR0: the effective address of the instruction that could not be fetched.
  uint32_t instruction_address = 0;
  // The effective address after little-endian munging.
  uint32_t storage_effective_address = 0;
  uint32_t backend_diagnostic = 0;

  bool is_guest_fault() const {
    return reason != GuestInstructionFetchFaultReason::kNone &&
           reason != GuestInstructionFetchFaultReason::kMappingRetired;
  }
};

// Reads PowerPC instruction words, in host byte order, for translation and the
// debugger. A failed read fetched nothing; only TryReadInstructionWithFault
// says whether it was a guest fault. A null output pointer fails.
class GuestCodeReader {
 public:
  virtual ~GuestCodeReader() = default;

  // The memory-access mode the reader implements.
  virtual MemoryAccessMode memory_access_mode() const {
    return MemoryAccessMode::kDirect;
  }

  virtual bool TryReadInstruction(uint32_t guest_address,
                                  uint32_t* instruction_out) const = 0;

  // Also reports the guest physical address the word was fetched from, so
  // translations can be invalidated when those bytes change. Readers without
  // a physical source report kUnknownInstructionSource.
  virtual bool TryReadInstructionWithSource(
      uint32_t guest_address, uint32_t* instruction_out,
      uint32_t* source_address_out) const {
    if (!source_address_out) {
      return false;
    }
    *source_address_out = kUnknownInstructionSource;
    return TryReadInstruction(guest_address, instruction_out);
  }

  // Also reports why a fetch failed. Readers without fault information report
  // GuestInstructionFetchFaultReason::kNone.
  virtual bool TryReadInstructionWithFault(
      uint32_t guest_address, uint32_t* instruction_out,
      uint32_t* source_address_out,
      GuestInstructionFetchFault* fault_out) const {
    if (!fault_out) {
      return false;
    }
    *fault_out = {};
    fault_out->instruction_address = guest_address;
    return TryReadInstructionWithSource(guest_address, instruction_out,
                                        source_address_out);
  }
};

// Reads flat guest memory through Memory::TranslateVirtual.
class MemoryGuestCodeReader final : public GuestCodeReader {
 public:
  explicit MemoryGuestCodeReader(Memory* memory) : memory_(memory) {}

  bool TryReadInstruction(uint32_t guest_address,
                          uint32_t* instruction_out) const override {
    if (!instruction_out) {
      return false;
    }
    *instruction_out =
        xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(guest_address));
    return true;
  }

 private:
  Memory* memory_;
};

}  // namespace xe::cpu::ppc

#endif  // XENIA_CPU_PPC_GUEST_CODE_READER_H_

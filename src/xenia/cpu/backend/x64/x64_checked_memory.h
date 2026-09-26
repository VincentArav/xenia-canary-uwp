/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_CHECKED_MEMORY_H_
#define XENIA_CPU_BACKEND_X64_X64_CHECKED_MEMORY_H_

#include <array>
#include <cstdint>

#include "xenia/cpu/guest_memory_access.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe::cpu {
class TranslationDomainDescriptor;
namespace hir {
class Instr;
}  // namespace hir
}  // namespace xe::cpu

namespace xe::cpu::backend::x64 {

class X64Emitter;

// Scratch record in the 48-byte x64 guest scratch area. primary_bytes is the
// input of writes and the output of reads; compare_bytes is used only by
// compare-exchange.
struct CheckedGuestMemoryCall {
  uint32_t effective_address = 0;
  uint32_t byte_count = 0;
  std::array<uint8_t, 16> primary_bytes{};
  std::array<uint8_t, 8> compare_bytes{};
  uint8_t predicate = 0;
  std::array<uint8_t, 7> reserved{};
};
static_assert(sizeof(CheckedGuestMemoryCall) == 40);

constexpr uint64_t PackCheckedGuestMemorySite(
    GuestMemoryAccessOperation operation, GuestMemoryOperandType operand_type,
    uint32_t instruction_address) noexcept {
  return static_cast<uint64_t>(operation) |
         (static_cast<uint64_t>(operand_type) << 8) |
         (static_cast<uint64_t>(instruction_address) << 32);
}

// Called through the guest-to-host thunk. Writes the result to call, or hands
// a fault to the Processor's checked-memory dispatcher, which doesn't return.
// Not noexcept: POSIX guest reentry unwinds through it.
uint64_t ExecuteCheckedGuestMemoryCall(
    ppc::PPCContext* context,
    const TranslationDomainDescriptor* translation_domain, uint64_t packed_site,
    CheckedGuestMemoryCall* call);

// Emits the checked sequence of an OPCODE_FLAG_MEMORY instruction, or returns
// false. Never falls back to the flat-memory sequences.
bool SelectCheckedGuestMemorySequence(X64Emitter* emitter,
                                      const hir::Instr* instruction);

}  // namespace xe::cpu::backend::x64

#endif  // XENIA_CPU_BACKEND_X64_X64_CHECKED_MEMORY_H_

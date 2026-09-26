/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_checked_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/x64/x64_backend.h"
#include "xenia/cpu/backend/x64/x64_emitter.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/checked_tlb.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/opcodes.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/translation_domain_registry.h"
#include "xenia/memory.h"

DEFINE_bool(x64_inline_checked_tlb, true,
            "Checked-domain scalar loads and stores probe the thread's cached "
            "translations inline and call the host only on a miss.",
            "x64");

namespace xe::cpu::backend::x64 {
namespace {

using hir::Instr;
using hir::TypeName;
using hir::Value;

constexpr size_t kCallStackOffset = X64Emitter::kStashOffset;
constexpr size_t kCallEffectiveAddressOffset =
    kCallStackOffset + offsetof(CheckedGuestMemoryCall, effective_address);
constexpr size_t kCallByteCountOffset =
    kCallStackOffset + offsetof(CheckedGuestMemoryCall, byte_count);
constexpr size_t kCallPrimaryBytesOffset =
    kCallStackOffset + offsetof(CheckedGuestMemoryCall, primary_bytes);
constexpr size_t kCallCompareBytesOffset =
    kCallStackOffset + offsetof(CheckedGuestMemoryCall, compare_bytes);
constexpr size_t kCallPredicateOffset =
    kCallStackOffset + offsetof(CheckedGuestMemoryCall, predicate);
static_assert(kCallStackOffset + sizeof(CheckedGuestMemoryCall) <=
              StackLayout::GUEST_PROFILER_START);

std::optional<GuestMemoryAccessOperation> UnpackOperation(
    uint64_t packed_site) noexcept {
  const auto operation =
      static_cast<GuestMemoryAccessOperation>(packed_site & 0xFF);
  switch (operation) {
    case GuestMemoryAccessOperation::kLoad:
    case GuestMemoryAccessOperation::kStore:
    case GuestMemoryAccessOperation::kLoadVectorLeft:
    case GuestMemoryAccessOperation::kLoadVectorRight:
    case GuestMemoryAccessOperation::kStoreVectorLeft:
    case GuestMemoryAccessOperation::kStoreVectorRight:
    case GuestMemoryAccessOperation::kMemset:
    case GuestMemoryAccessOperation::kAtomicCompareExchange:
    case GuestMemoryAccessOperation::kLoadReserve:
    case GuestMemoryAccessOperation::kStoreConditional:
      return operation;
  }
  return std::nullopt;
}

std::optional<GuestMemoryOperandType> UnpackOperandType(
    uint64_t packed_site) noexcept {
  const auto operand_type =
      static_cast<GuestMemoryOperandType>((packed_site >> 8) & 0xFF);
  return GuestMemoryOperandByteCount(operand_type) != 0
             ? std::optional<GuestMemoryOperandType>(operand_type)
             : std::nullopt;
}

uint32_t UnpackInstructionAddress(uint64_t packed_site) noexcept {
  return static_cast<uint32_t>(packed_site >> 32);
}

[[noreturn]] void DeliverCheckedMemoryFault(
    ppc::PPCContext* context,
    const TranslationDomainDescriptor* translation_domain,
    const GuestMemoryAccessRequest& request,
    const GuestMemoryAccessResult& result) {
  if (context && context->processor && translation_domain) {
    if (CheckedGuestMemoryDispatcher* dispatcher =
            context->processor->checked_guest_memory_dispatcher()) {
      dispatcher->HandleGuestMemoryFault(
          context, translation_domain->translation_domain_key(), request,
          result);
    }
  }
  // Dispatchers don't return from a checked fault.
  std::abort();
}

GuestMemoryAccessResult InvalidRuntimeRequest(
    const GuestMemoryAccessRequest& request) noexcept {
  GuestMemoryAccessResult result;
  result.diagnostic = GuestMemoryAccessDiagnostic::kInvalidRequest;
  result.fault.reason = GuestMemoryAccessFaultReason::kAlignment;
  result.fault.stage = GuestMemoryAccessFaultStage::kRequest;
  result.fault.instruction_address = request.instruction_address;
  result.fault.effective_address = request.effective_address;
  result.fault.byte_count = request.byte_count;
  return result;
}

bool IsWriteOperation(GuestMemoryAccessOperation operation) noexcept {
  switch (operation) {
    case GuestMemoryAccessOperation::kStore:
    case GuestMemoryAccessOperation::kStoreVectorLeft:
    case GuestMemoryAccessOperation::kStoreVectorRight:
    case GuestMemoryAccessOperation::kMemset:
    case GuestMemoryAccessOperation::kAtomicCompareExchange:
    case GuestMemoryAccessOperation::kStoreConditional:
      return true;
    default:
      return false;
  }
}

constexpr bool IsCheckedCacheControlNoOp(uint32_t flags) noexcept {
  switch (static_cast<hir::CacheControlType>(flags)) {
    case hir::CACHE_CONTROL_TYPE_DATA_TOUCH:
    case hir::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
    case hir::CACHE_CONTROL_TYPE_DATA_STORE:
    case hir::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
      return true;
    default:
      return false;
  }
}
static_assert(IsCheckedCacheControlNoOp(hir::CACHE_CONTROL_TYPE_DATA_TOUCH));
static_assert(
    IsCheckedCacheControlNoOp(hir::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE));
static_assert(IsCheckedCacheControlNoOp(hir::CACHE_CONTROL_TYPE_DATA_STORE));
static_assert(
    IsCheckedCacheControlNoOp(hir::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH));
static_assert(!IsCheckedCacheControlNoOp(UINT32_MAX));

X64BackendContext* BackendContextFor(ppc::PPCContext* context) noexcept {
  return reinterpret_cast<X64BackendContext*>(
      reinterpret_cast<uint8_t*>(context) - sizeof(X64BackendContext));
}

std::optional<GuestMemoryOperandType> OperandTypeFor(TypeName type) noexcept {
  switch (type) {
    case hir::INT8_TYPE:
      return GuestMemoryOperandType::kInt8;
    case hir::INT16_TYPE:
      return GuestMemoryOperandType::kInt16;
    case hir::INT32_TYPE:
      return GuestMemoryOperandType::kInt32;
    case hir::INT64_TYPE:
      return GuestMemoryOperandType::kInt64;
    case hir::FLOAT32_TYPE:
      return GuestMemoryOperandType::kFloat32;
    case hir::FLOAT64_TYPE:
      return GuestMemoryOperandType::kFloat64;
    case hir::VEC128_TYPE:
      return GuestMemoryOperandType::kVector128;
    default:
      return std::nullopt;
  }
}

bool Supports(const TranslationDomainDescriptor& descriptor,
              GuestMemoryAccessOperation operation,
              GuestMemoryOperandType operand_type) noexcept {
  const GuestMemoryAccessor* const accessor =
      descriptor.guest_memory_accessor();
  if (!accessor ||
      accessor->memory_access_mode() != MemoryAccessMode::kChecked) {
    return false;
  }
  GuestMemoryAccessRequest request;
  request.operation = operation;
  request.operand_type = operand_type;
  request.byte_count = operation == GuestMemoryAccessOperation::kMemset
                           ? 1
                           : GuestMemoryOperandByteCount(operand_type);
  request.reservation_identity_valid =
      operation == GuestMemoryAccessOperation::kStoreConditional;
  return GuestMemoryAccessCapabilitiesSupport(accessor->capabilities(),
                                              request);
}

void EmitValueToEax(X64Emitter& e, const Value* value) {
  if (value->IsConstant()) {
    e.mov(e.eax, value->constant.u32);
    return;
  }
  Xbyak::Reg64 source;
  X64Emitter::SetupReg(value, source);
  e.mov(e.eax, source.cvt32());
}

// The effective address into eax (zero-extended into rax).
void ComputeEffectiveAddress(X64Emitter& e, const Value* base,
                             const Value* offset) {
  EmitValueToEax(e, base);
  if (offset) {
    if (offset->IsConstant()) {
      e.add(e.eax, offset->constant.u32);
    } else {
      Xbyak::Reg64 offset_reg;
      X64Emitter::SetupReg(offset, offset_reg);
      e.add(e.eax, offset_reg.cvt32());
    }
  }
}

void EmitEffectiveAddress(X64Emitter& e, const Value* base,
                          const Value* offset = nullptr) {
  ComputeEffectiveAddress(e, base, offset);
  e.mov(e.dword[e.rsp + kCallEffectiveAddressOffset], e.eax);
}

uint64_t ScalarConstantBits(const Value& value) noexcept {
  switch (value.type) {
    case hir::INT8_TYPE:
      return value.constant.u8;
    case hir::INT16_TYPE:
      return value.constant.u16;
    case hir::INT32_TYPE:
      return value.constant.u32;
    case hir::INT64_TYPE:
      return value.constant.u64;
    case hir::FLOAT32_TYPE:
      return value.constant.u32;
    case hir::FLOAT64_TYPE:
      return value.constant.u64;
    default:
      return 0;
  }
}

void EmitScalarInput(X64Emitter& e, const Value* value, size_t stack_offset,
                     bool byte_swap) {
  const size_t byte_count = hir::GetTypeSize(value->type);
  if (value->IsConstant()) {
    uint64_t bits = ScalarConstantBits(*value);
    if (byte_swap) {
      switch (byte_count) {
        case 2:
          bits = xe::byte_swap(static_cast<uint16_t>(bits));
          break;
        case 4:
          bits = xe::byte_swap(static_cast<uint32_t>(bits));
          break;
        case 8:
          bits = xe::byte_swap(bits);
          break;
      }
    }
    switch (byte_count) {
      case 1:
        e.mov(e.byte[e.rsp + stack_offset], static_cast<uint8_t>(bits));
        return;
      case 2:
        e.mov(e.word[e.rsp + stack_offset], static_cast<uint16_t>(bits));
        return;
      case 4:
        e.mov(e.dword[e.rsp + stack_offset], static_cast<uint32_t>(bits));
        return;
      case 8:
        e.MovMem64(e.rsp + stack_offset, bits);
        return;
    }
  }

  if (value->type == hir::FLOAT32_TYPE || value->type == hir::FLOAT64_TYPE) {
    Xbyak::Xmm source;
    X64Emitter::SetupReg(value, source);
    if (byte_count == 4) {
      e.vmovd(e.eax, source);
      if (byte_swap) {
        e.bswap(e.eax);
      }
      e.mov(e.dword[e.rsp + stack_offset], e.eax);
    } else {
      e.vmovq(e.rax, source);
      if (byte_swap) {
        e.bswap(e.rax);
      }
      e.mov(e.qword[e.rsp + stack_offset], e.rax);
    }
    return;
  }

  Xbyak::Reg64 source;
  X64Emitter::SetupReg(value, source);
  switch (byte_count) {
    case 1:
      e.mov(e.byte[e.rsp + stack_offset], source.cvt8());
      break;
    case 2:
      if (byte_swap) {
        e.movzx(e.eax, source.cvt16());
        e.ror(e.ax, 8);
        e.mov(e.word[e.rsp + stack_offset], e.ax);
      } else {
        e.mov(e.word[e.rsp + stack_offset], source.cvt16());
      }
      break;
    case 4:
      e.mov(e.eax, source.cvt32());
      if (byte_swap) {
        e.bswap(e.eax);
      }
      e.mov(e.dword[e.rsp + stack_offset], e.eax);
      break;
    case 8:
      e.mov(e.rax, source);
      if (byte_swap) {
        e.bswap(e.rax);
      }
      e.mov(e.qword[e.rsp + stack_offset], e.rax);
      break;
  }
}

void EmitVectorInput(X64Emitter& e, const Value* value, size_t stack_offset,
                     bool byte_swap) {
  Xbyak::Xmm source;
  if (value->IsConstant()) {
    e.LoadConstantXmm(e.xmm0, value->constant.v128);
    source = e.xmm0;
  } else {
    X64Emitter::SetupReg(value, source);
  }
  if (byte_swap) {
    e.vpshufb(e.xmm0, source, e.GetXmmConstPtr(XMMByteSwapMask));
    source = e.xmm0;
  }
  e.vmovdqu(e.ptr[e.rsp + stack_offset], source);
}

void EmitInput(X64Emitter& e, const Value* value, size_t stack_offset,
               bool byte_swap) {
  if (value->type == hir::VEC128_TYPE) {
    EmitVectorInput(e, value, stack_offset, byte_swap);
  } else {
    EmitScalarInput(e, value, stack_offset, byte_swap);
  }
}

void EmitScalarOutput(X64Emitter& e, Value* destination, bool byte_swap) {
  const size_t byte_count = hir::GetTypeSize(destination->type);
  if (destination->type == hir::FLOAT32_TYPE ||
      destination->type == hir::FLOAT64_TYPE) {
    Xbyak::Xmm output;
    X64Emitter::SetupReg(destination, output);
    if (byte_count == 4) {
      e.mov(e.eax, e.dword[e.rsp + kCallPrimaryBytesOffset]);
      if (byte_swap) {
        e.bswap(e.eax);
      }
      e.vmovd(output, e.eax);
    } else {
      e.mov(e.rax, e.qword[e.rsp + kCallPrimaryBytesOffset]);
      if (byte_swap) {
        e.bswap(e.rax);
      }
      e.vmovq(output, e.rax);
    }
    return;
  }

  Xbyak::Reg64 output;
  X64Emitter::SetupReg(destination, output);
  switch (byte_count) {
    case 1:
      e.mov(output.cvt8(), e.byte[e.rsp + kCallPrimaryBytesOffset]);
      break;
    case 2:
      e.mov(output.cvt16(), e.word[e.rsp + kCallPrimaryBytesOffset]);
      if (byte_swap) {
        e.ror(output.cvt16(), 8);
      }
      break;
    case 4:
      e.mov(output.cvt32(), e.dword[e.rsp + kCallPrimaryBytesOffset]);
      if (byte_swap) {
        e.bswap(output.cvt32());
      }
      break;
    case 8:
      e.mov(output, e.qword[e.rsp + kCallPrimaryBytesOffset]);
      if (byte_swap) {
        e.bswap(output);
      }
      break;
  }
}

void EmitVectorOutput(X64Emitter& e, Value* destination, bool byte_swap) {
  Xbyak::Xmm output;
  X64Emitter::SetupReg(destination, output);
  e.vmovdqu(output, e.ptr[e.rsp + kCallPrimaryBytesOffset]);
  if (byte_swap) {
    e.vpshufb(output, output, e.GetXmmConstPtr(XMMByteSwapMask));
  }
}

void EmitOutput(X64Emitter& e, Value* destination, bool byte_swap) {
  if (destination->type == hir::VEC128_TYPE) {
    EmitVectorOutput(e, destination, byte_swap);
  } else {
    EmitScalarOutput(e, destination, byte_swap);
  }
}

void EmitRuntimeCall(X64Emitter& e,
                     const TranslationDomainDescriptor& descriptor,
                     GuestMemoryAccessOperation operation,
                     GuestMemoryOperandType operand_type,
                     uint32_t instruction_address) {
  e.mov(e.GetNativeParam(0), reinterpret_cast<uint64_t>(&descriptor));
  e.mov(e.GetNativeParam(1), PackCheckedGuestMemorySite(operation, operand_type,
                                                        instruction_address));
  e.lea(e.GetNativeParam(2), e.ptr[e.rsp + kCallStackOffset]);
  e.CallNativeSafe(reinterpret_cast<void*>(ExecuteCheckedGuestMemoryCall));
}

// A displacement from the context register into its checked TLB.
int32_t CheckedTlbDisplacement(size_t offset_in_tlb) {
  return static_cast<int32_t>(
      -static_cast<ptrdiff_t>(sizeof(X64BackendContext)) +
      static_cast<ptrdiff_t>(offsetof(X64BackendContext, checked_tlb) +
                             offset_in_tlb));
}

// With the effective address in eax, leaves the entry's host_delta in rdx, or
// jumps to miss when the table is stale or has no usable entry of accessor_id
// for the access. The tag comes from the access's last byte and the index from
// its first, so an access crossing a page never matches. Clobbers rcx, rdx and
// flags.
void EmitCheckedTlbProbe(X64Emitter& e, bool write, uint32_t byte_count,
                         uint32_t accessor_id, Xbyak::Label& miss) {
  const Xbyak::Reg64 context = e.GetContextReg();
  e.cmp(e.dword[context + CheckedTlbDisplacement(offsetof(CheckedTlb, stale))],
        0);
  e.jne(miss, X64Emitter::T_NEAR);
  // The index, as CheckedTlb::IndexFor, scaled by the entry size.
  e.mov(e.edx, e.eax);
  e.shr(e.edx, kCheckedTlbPageShift);
  e.mov(e.ecx, e.edx);
  e.shr(e.ecx, kCheckedTlbIndexBits);
  e.add(e.edx, e.ecx);
  e.shl(e.edx, 4);
  e.and_(e.edx, (kCheckedTlbEntryCount - 1) << 4);
  if (byte_count > 1) {
    // 32-bit address arithmetic: the last byte wraps as the guest's does.
    e.lea(e.ecx, e.ptr[e.eax + (byte_count - 1)]);
  } else {
    e.mov(e.ecx, e.eax);
  }
  e.shr(e.ecx, kCheckedTlbPageShift);
  assert_true(accessor_id <= kCheckedTlbMaxPackedAccessorId);
  e.or_(e.ecx, kCheckedTlbValidBit | kCheckedTlbInlineBit |
                   (accessor_id << kCheckedTlbAccessorShift));
  static_assert(sizeof(CheckedTlbEntry) == 16);
  const int32_t table = CheckedTlbDisplacement(
      write ? offsetof(CheckedTlb, write) : offsetof(CheckedTlb, read));
  e.cmp(e.ecx, e.dword[context + e.rdx + table]);
  e.jne(miss, X64Emitter::T_NEAR);
  e.mov(e.rdx, e.qword[context + e.rdx +
                       (table + offsetof(CheckedTlbEntry, host_delta))]);
}

// A load from host_delta (rdx) + effective address (rax), the canonical
// big-endian bytes swapped exactly as EmitScalarOutput swaps them.
void EmitInlineLoad(X64Emitter& e, Value* destination, bool byte_swap) {
  const size_t byte_count = hir::GetTypeSize(destination->type);
  const bool movbe = byte_swap && e.IsFeatureEnabled(kX64EmitMovbe);
  const Xbyak::RegExp address = e.rdx + e.rax;
  if (destination->type == hir::FLOAT32_TYPE ||
      destination->type == hir::FLOAT64_TYPE) {
    Xbyak::Xmm output;
    X64Emitter::SetupReg(destination, output);
    const Xbyak::Reg32e scratch =
        byte_count == 4 ? static_cast<const Xbyak::Reg32e&>(e.ecx)
                        : static_cast<const Xbyak::Reg32e&>(e.rcx);
    const Xbyak::Address source =
        byte_count == 4 ? e.dword[address] : e.qword[address];
    if (movbe) {
      e.movbe(scratch, source);
    } else {
      e.mov(scratch, source);
      if (byte_swap) {
        e.bswap(scratch);
      }
    }
    if (byte_count == 4) {
      e.vmovd(output, e.ecx);
    } else {
      e.vmovq(output, e.rcx);
    }
    return;
  }
  Xbyak::Reg64 output;
  X64Emitter::SetupReg(destination, output);
  switch (byte_count) {
    case 1:
      e.mov(output.cvt8(), e.byte[address]);
      break;
    case 2:
      if (movbe) {
        e.movbe(output.cvt16(), e.word[address]);
      } else {
        e.mov(output.cvt16(), e.word[address]);
        if (byte_swap) {
          e.ror(output.cvt16(), 8);
        }
      }
      break;
    case 4:
      if (movbe) {
        e.movbe(output.cvt32(), e.dword[address]);
      } else {
        e.mov(output.cvt32(), e.dword[address]);
        if (byte_swap) {
          e.bswap(output.cvt32());
        }
      }
      break;
    case 8:
      if (movbe) {
        e.movbe(output, e.qword[address]);
      } else {
        e.mov(output, e.qword[address]);
        if (byte_swap) {
          e.bswap(output);
        }
      }
      break;
  }
}

// A store to host_delta (rdx) + effective address (rax), the bytes swapped
// exactly as EmitScalarInput swaps them. Clobbers rcx.
void EmitInlineStore(X64Emitter& e, const Value* value, bool byte_swap) {
  const size_t byte_count = hir::GetTypeSize(value->type);
  const Xbyak::RegExp address = e.rdx + e.rax;
  if (value->IsConstant()) {
    uint64_t bits = ScalarConstantBits(*value);
    if (byte_swap) {
      switch (byte_count) {
        case 2:
          bits = xe::byte_swap(static_cast<uint16_t>(bits));
          break;
        case 4:
          bits = xe::byte_swap(static_cast<uint32_t>(bits));
          break;
        case 8:
          bits = xe::byte_swap(bits);
          break;
      }
    }
    switch (byte_count) {
      case 1:
        e.mov(e.byte[address], static_cast<uint8_t>(bits));
        return;
      case 2:
        e.mov(e.word[address], static_cast<uint16_t>(bits));
        return;
      case 4:
        e.mov(e.dword[address], static_cast<uint32_t>(bits));
        return;
      case 8:
        e.mov(e.rcx, bits);
        e.mov(e.qword[address], e.rcx);
        return;
    }
  }

  const bool movbe = byte_swap && e.IsFeatureEnabled(kX64EmitMovbe);
  Xbyak::Reg source;
  if (value->type == hir::FLOAT32_TYPE || value->type == hir::FLOAT64_TYPE) {
    Xbyak::Xmm xmm_source;
    X64Emitter::SetupReg(value, xmm_source);
    if (byte_count == 4) {
      e.vmovd(e.ecx, xmm_source);
      source = e.ecx;
    } else {
      e.vmovq(e.rcx, xmm_source);
      source = e.rcx;
    }
  } else {
    Xbyak::Reg64 gpr_source;
    X64Emitter::SetupReg(value, gpr_source);
    switch (byte_count) {
      case 1:
        e.mov(e.byte[address], gpr_source.cvt8());
        return;
      case 2:
        if (!byte_swap) {
          e.mov(e.word[address], gpr_source.cvt16());
        } else if (movbe) {
          e.movbe(e.word[address], gpr_source.cvt16());
        } else {
          e.movzx(e.ecx, gpr_source.cvt16());
          e.ror(e.cx, 8);
          e.mov(e.word[address], e.cx);
        }
        return;
      case 4:
        source = gpr_source.cvt32();
        break;
      case 8:
        source = gpr_source;
        break;
    }
  }
  const Xbyak::Address destination =
      byte_count == 4 ? e.dword[address] : e.qword[address];
  if (!byte_swap) {
    e.mov(destination, source);
  } else if (movbe) {
    e.movbe(destination, source);
  } else {
    const Xbyak::Reg32e scratch =
        byte_count == 4 ? static_cast<const Xbyak::Reg32e&>(e.ecx)
                        : static_cast<const Xbyak::Reg32e&>(e.rcx);
    if (source.getIdx() != scratch.getIdx()) {
      e.mov(scratch, source);
    }
    e.bswap(scratch);
    e.mov(destination, scratch);
  }
}

// An aligned 128-bit load or store through host_delta (rdx) + effective
// address (rax), swapped as EmitVectorOutput and EmitVectorInput swap it.
void EmitInlineVectorLoad(X64Emitter& e, Value* destination, bool byte_swap) {
  Xbyak::Xmm output;
  X64Emitter::SetupReg(destination, output);
  e.vmovdqu(output, e.ptr[e.rdx + e.rax]);
  if (byte_swap) {
    e.vpshufb(output, output, e.GetXmmConstPtr(XMMByteSwapMask));
  }
}

void EmitInlineVectorStore(X64Emitter& e, const Value* value, bool byte_swap) {
  Xbyak::Xmm source;
  if (value->IsConstant()) {
    // Through the stash only; rax and rdx are live.
    e.LoadConstantXmm(e.xmm0, value->constant.v128);
    source = e.xmm0;
  } else {
    X64Emitter::SetupReg(value, source);
  }
  if (byte_swap) {
    e.vpshufb(e.xmm0, source, e.GetXmmConstPtr(XMMByteSwapMask));
    source = e.xmm0;
  }
  e.vmovdqu(e.ptr[e.rdx + e.rax], source);
}

// lvlx/lvrx vpshufb controls, per byte offset: each moves the aligned block's
// bytes into place and into Xenia's x64 lane order (bytes reversed within each
// 32-bit lane) at once.
struct VectorLeftRightMasks {
  alignas(16) uint8_t left[16][16];
  alignas(16) uint8_t right[16][16];

  VectorLeftRightMasks() {
    for (int n = 0; n < 16; ++n) {
      for (int k = 0; k < 16; ++k) {
        const int j = k ^ 3;
        left[n][k] = j + n < 16 ? uint8_t(j + n) : uint8_t(0x80);
        right[n][k] = j >= 16 - n ? uint8_t(j - 16 + n) : uint8_t(0x80);
      }
    }
  }
};
const VectorLeftRightMasks vector_left_right_masks;

void EmitValueToEcx(X64Emitter& e, const Value* value) {
  if (value->IsConstant()) {
    e.mov(e.ecx, value->constant.u32);
    return;
  }
  Xbyak::Reg64 source;
  X64Emitter::SetupReg(value, source);
  e.mov(e.ecx, source.cvt32());
}

// After a host call the guest-to-host thunk leaves MXCSR in FPU mode; the
// code after the access expects the mode it was emitted for.
void RestoreMxcsrMode(X64Emitter& e, MXCSRMode mode) {
  switch (mode) {
    case MXCSRMode::Vmx:
      e.LoadVmxMxcsrDirect();
      break;
    case MXCSRMode::Unknown: {
      Xbyak::Label& fpu = e.NewCachedLabel();
      e.bt(e.GetBackendFlagsPtr(), kX64BackendMXCSRModeBit);
      e.jnc(fpu, X64Emitter::T_NEAR);
      e.LoadVmxMxcsrDirect();
      e.L(fpu);
      break;
    }
    case MXCSRMode::Fpu:
      break;
  }
}

bool EmitLoadStore(X64Emitter& e, const Instr& instruction,
                   GuestMemoryAccessOperation operation, Value* value,
                   const Value* base, const Value* offset) {
  const std::optional<GuestMemoryOperandType> operand_type =
      OperandTypeFor(value->type);
  const TranslationDomainDescriptor* const descriptor =
      e.checked_memory_descriptor();
  if (!operand_type.has_value() || !descriptor ||
      !Supports(*descriptor, operation, *operand_type)) {
    return false;
  }

  const uint32_t byte_count = GuestMemoryOperandByteCount(*operand_type);
  const bool byte_swap = (instruction.flags & hir::LOAD_STORE_BYTE_SWAP) != 0;
  const bool is_vector = *operand_type == GuestMemoryOperandType::kVector128;
  const uint32_t accessor_id =
      cvars::x64_inline_checked_tlb
          ? descriptor->guest_memory_accessor()->inline_tlb_accessor_id()
          : 0;
  if (accessor_id) {
    // A hit completes inline; anything else takes the host call below.
    const bool is_store = operation == GuestMemoryAccessOperation::kStore;
    Xbyak::Label& miss = e.NewCachedLabel();
    Xbyak::Label& done = e.NewCachedLabel();
    ComputeEffectiveAddress(e, base, offset);
    if (is_vector) {
      // As TryFastLoad/TryFastStore, only aligned vectors.
      e.test(e.al, 15);
      e.jnz(miss, X64Emitter::T_NEAR);
    }
    EmitCheckedTlbProbe(e, is_store, byte_count, accessor_id, miss);
    if (is_store) {
      if (is_vector) {
        EmitInlineVectorStore(e, value, byte_swap);
      } else {
        EmitInlineStore(e, value, byte_swap);
      }
      // A store ends this thread's reservation.
      e.and_(e.GetBackendFlagsPtr(),
             ~(uint32_t{1} << kX64BackendHasCheckedReserveBit));
    } else if (is_vector) {
      EmitInlineVectorLoad(e, value, byte_swap);
    } else {
      EmitInlineLoad(e, value, byte_swap);
    }
    e.L(done);
    const MXCSRMode mxcsr_mode = e.mxcsr_mode();
    const GuestMemoryOperandType type = *operand_type;
    const uint32_t guest_address = instruction.GuestAddressFor();
    e.AddToTail([=, &miss, &done](X64Emitter& e, Xbyak::Label& tail) {
      e.L(tail);
      e.L(miss);
      e.mov(e.dword[e.rsp + kCallEffectiveAddressOffset], e.eax);
      e.mov(e.dword[e.rsp + kCallByteCountOffset], byte_count);
      if (is_store) {
        EmitInput(e, value, kCallPrimaryBytesOffset, byte_swap);
      }
      EmitRuntimeCall(e, *descriptor, operation, type, guest_address);
      if (!is_store) {
        EmitOutput(e, value, byte_swap);
      }
      RestoreMxcsrMode(e, mxcsr_mode);
      e.jmp(done, X64Emitter::T_NEAR);
    });
    return true;
  }

  EmitEffectiveAddress(e, base, offset);
  e.mov(e.dword[e.rsp + kCallByteCountOffset], byte_count);
  if (operation == GuestMemoryAccessOperation::kStore) {
    EmitInput(e, value, kCallPrimaryBytesOffset, byte_swap);
  }
  EmitRuntimeCall(e, *descriptor, operation, *operand_type,
                  instruction.GuestAddressFor());
  if (operation == GuestMemoryAccessOperation::kLoad) {
    EmitOutput(e, value, byte_swap);
  }
  return true;
}

bool EmitVectorLeftRight(X64Emitter& e, const Instr& instruction,
                         GuestMemoryAccessOperation operation) {
  const TranslationDomainDescriptor* const descriptor =
      e.checked_memory_descriptor();
  if (!descriptor ||
      !Supports(*descriptor, operation, GuestMemoryOperandType::kVector128)) {
    return false;
  }
  const bool is_store =
      operation == GuestMemoryAccessOperation::kStoreVectorLeft ||
      operation == GuestMemoryAccessOperation::kStoreVectorRight;
  const uint32_t accessor_id =
      cvars::x64_inline_checked_tlb && !is_store
          ? descriptor->guest_memory_accessor()->inline_tlb_accessor_id()
          : 0;
  if (accessor_id) {
    // A hit reads the aligned block, which a 16-byte access never extends
    // past, and shuffles its bytes into place.
    const Value* const address = instruction.src1.value;
    Value* const destination = instruction.dest;
    Xbyak::Label& miss = e.NewCachedLabel();
    Xbyak::Label& done = e.NewCachedLabel();
    ComputeEffectiveAddress(e, address, nullptr);
    e.and_(e.eax, ~uint32_t(15));
    EmitCheckedTlbProbe(e, false, 16, accessor_id, miss);
    e.vmovdqu(e.xmm0, e.ptr[e.rdx + e.rax]);
    EmitValueToEcx(e, address);
    e.and_(e.ecx, 15);
    e.shl(e.ecx, 4);
    e.mov(e.rdx, reinterpret_cast<uint64_t>(
                     operation == GuestMemoryAccessOperation::kLoadVectorLeft
                         ? &vector_left_right_masks.left[0][0]
                         : &vector_left_right_masks.right[0][0]));
    Xbyak::Xmm output;
    X64Emitter::SetupReg(destination, output);
    e.vpshufb(output, e.xmm0, e.ptr[e.rdx + e.rcx]);
    e.L(done);
    const MXCSRMode mxcsr_mode = e.mxcsr_mode();
    const uint32_t guest_address = instruction.GuestAddressFor();
    e.AddToTail([=, &miss, &done](X64Emitter& e, Xbyak::Label& tail) {
      e.L(tail);
      e.L(miss);
      EmitEffectiveAddress(e, address);
      e.mov(e.dword[e.rsp + kCallByteCountOffset], 16);
      EmitRuntimeCall(e, *descriptor, operation,
                      GuestMemoryOperandType::kVector128, guest_address);
      EmitVectorOutput(e, destination, true);
      RestoreMxcsrMode(e, mxcsr_mode);
      e.jmp(done, X64Emitter::T_NEAR);
    });
    return true;
  }
  EmitEffectiveAddress(e, instruction.src1.value);
  e.mov(e.dword[e.rsp + kCallByteCountOffset], 16);
  if (is_store) {
    // The CPU-neutral boundary uses architectural vector byte order. Xenia's
    // x64 vec128 representation reverses bytes within each 32-bit lane.
    EmitVectorInput(e, instruction.src2.value, kCallPrimaryBytesOffset, true);
  }
  EmitRuntimeCall(e, *descriptor, operation, GuestMemoryOperandType::kVector128,
                  instruction.GuestAddressFor());
  if (!is_store) {
    EmitVectorOutput(e, instruction.dest, true);
  }
  return true;
}

bool EmitMemset(X64Emitter& e, const Instr& instruction) {
  const TranslationDomainDescriptor* const descriptor =
      e.checked_memory_descriptor();
  if (!descriptor || !Supports(*descriptor, GuestMemoryAccessOperation::kMemset,
                               GuestMemoryOperandType::kInt8)) {
    return false;
  }
  const Value* const length = instruction.src3.value;
  const Value* const fill = instruction.src2.value;
  const uint32_t accessor_id =
      cvars::x64_inline_checked_tlb && length->IsConstant() &&
              fill->IsConstant() &&
              (length->constant.u32 == 32 || length->constant.u32 == 128)
          ? descriptor->guest_memory_accessor()->inline_tlb_accessor_id()
          : 0;
  if (accessor_id) {
    // dcbz: an aligned line, which never extends past its page.
    const Value* const address = instruction.src1.value;
    const uint32_t byte_count = length->constant.u32;
    const uint8_t fill_byte = fill->constant.u8;
    Xbyak::Label& miss = e.NewCachedLabel();
    Xbyak::Label& done = e.NewCachedLabel();
    ComputeEffectiveAddress(e, address, nullptr);
    e.test(e.eax, byte_count - 1);
    e.jnz(miss, X64Emitter::T_NEAR);
    EmitCheckedTlbProbe(e, true, byte_count, accessor_id, miss);
    if (fill_byte) {
      e.mov(e.ecx, uint32_t(fill_byte) * 0x01010101u);
      e.vmovd(e.xmm0, e.ecx);
      e.vpshufd(e.xmm0, e.xmm0, 0);
    } else {
      e.vpxor(e.xmm0, e.xmm0, e.xmm0);
    }
    for (uint32_t i = 0; i < byte_count; i += 16) {
      e.vmovdqu(e.ptr[e.rdx + e.rax + i], e.xmm0);
    }
    // A store ends this thread's reservation.
    e.and_(e.GetBackendFlagsPtr(),
           ~(uint32_t{1} << kX64BackendHasCheckedReserveBit));
    e.L(done);
    const MXCSRMode mxcsr_mode = e.mxcsr_mode();
    const uint32_t guest_address = instruction.GuestAddressFor();
    e.AddToTail([=, &miss, &done](X64Emitter& e, Xbyak::Label& tail) {
      e.L(tail);
      e.L(miss);
      e.mov(e.dword[e.rsp + kCallEffectiveAddressOffset], e.eax);
      e.mov(e.dword[e.rsp + kCallByteCountOffset], byte_count);
      EmitScalarInput(e, fill, kCallPrimaryBytesOffset, false);
      EmitRuntimeCall(e, *descriptor, GuestMemoryAccessOperation::kMemset,
                      GuestMemoryOperandType::kInt8, guest_address);
      RestoreMxcsrMode(e, mxcsr_mode);
      e.jmp(done, X64Emitter::T_NEAR);
    });
    return true;
  }
  EmitEffectiveAddress(e, instruction.src1.value);
  EmitValueToEax(e, instruction.src3.value);
  e.mov(e.dword[e.rsp + kCallByteCountOffset], e.eax);
  EmitScalarInput(e, instruction.src2.value, kCallPrimaryBytesOffset, false);
  EmitRuntimeCall(e, *descriptor, GuestMemoryAccessOperation::kMemset,
                  GuestMemoryOperandType::kInt8, instruction.GuestAddressFor());
  return true;
}

bool EmitAtomicCompareExchange(X64Emitter& e, const Instr& instruction) {
  const std::optional<GuestMemoryOperandType> operand_type =
      OperandTypeFor(instruction.src2.value->type);
  const TranslationDomainDescriptor* const descriptor =
      e.checked_memory_descriptor();
  if (!operand_type.has_value() ||
      (*operand_type != GuestMemoryOperandType::kInt32 &&
       *operand_type != GuestMemoryOperandType::kInt64) ||
      instruction.src3.value->type != instruction.src2.value->type ||
      !descriptor ||
      !Supports(*descriptor, GuestMemoryAccessOperation::kAtomicCompareExchange,
                *operand_type)) {
    return false;
  }
  const uint32_t byte_count = GuestMemoryOperandByteCount(*operand_type);
  EmitEffectiveAddress(e, instruction.src1.value);
  e.mov(e.dword[e.rsp + kCallByteCountOffset], byte_count);
  EmitInput(e, instruction.src3.value, kCallPrimaryBytesOffset, false);
  EmitInput(e, instruction.src2.value, kCallCompareBytesOffset, false);
  EmitRuntimeCall(e, *descriptor,
                  GuestMemoryAccessOperation::kAtomicCompareExchange,
                  *operand_type, instruction.GuestAddressFor());
  Xbyak::Reg8 output;
  X64Emitter::SetupReg(instruction.dest, output);
  e.mov(output, e.byte[e.rsp + kCallPredicateOffset]);
  return true;
}

bool EmitReservation(X64Emitter& e, const Instr& instruction,
                     GuestMemoryAccessOperation operation) {
  Value* const value = operation == GuestMemoryAccessOperation::kLoadReserve
                           ? instruction.dest
                           : instruction.src2.value;
  const std::optional<GuestMemoryOperandType> operand_type =
      OperandTypeFor(value->type);
  const TranslationDomainDescriptor* const descriptor =
      e.checked_memory_descriptor();
  if (!operand_type.has_value() ||
      (*operand_type != GuestMemoryOperandType::kInt32 &&
       *operand_type != GuestMemoryOperandType::kInt64) ||
      !descriptor || !Supports(*descriptor, operation, *operand_type)) {
    return false;
  }
  const uint32_t byte_count = GuestMemoryOperandByteCount(*operand_type);
  EmitEffectiveAddress(e, instruction.src1.value);
  e.mov(e.dword[e.rsp + kCallByteCountOffset], byte_count);
  if (operation == GuestMemoryAccessOperation::kStoreConditional) {
    EmitInput(e, value, kCallPrimaryBytesOffset, false);
  }
  EmitRuntimeCall(e, *descriptor, operation, *operand_type,
                  instruction.GuestAddressFor());
  if (operation == GuestMemoryAccessOperation::kLoadReserve) {
    EmitOutput(e, value, false);
  } else {
    Xbyak::Reg8 output;
    X64Emitter::SetupReg(instruction.dest, output);
    e.mov(output, e.byte[e.rsp + kCallPredicateOffset]);
  }
  return true;
}

}  // namespace

uint64_t ExecuteCheckedGuestMemoryCall(
    ppc::PPCContext* context,
    const TranslationDomainDescriptor* translation_domain, uint64_t packed_site,
    CheckedGuestMemoryCall* call) {
  const std::optional<GuestMemoryAccessOperation> operation =
      UnpackOperation(packed_site);
  // Plain loads and stores through a cached translation skip the full
  // request.
  CheckedTlb* const checked_tlb =
      context ? &BackendContextFor(context)->checked_tlb : nullptr;
  if (operation && call && context && translation_domain && call->byte_count &&
      call->byte_count <= call->primary_bytes.size()) {
    const GuestMemoryAccessor* const accessor =
        translation_domain->guest_memory_accessor();
    if (*operation == GuestMemoryAccessOperation::kLoad && accessor &&
        accessor->TryFastLoad(
            call->effective_address,
            std::span<uint8_t>(call->primary_bytes.data(), call->byte_count),
            checked_tlb)) {
      call->predicate = 0;
      return 1;
    }
    if (*operation == GuestMemoryAccessOperation::kStore && accessor &&
        accessor->TryFastStore(
            call->effective_address,
            std::span<const uint8_t>(call->primary_bytes.data(),
                                     call->byte_count),
            checked_tlb)) {
      // A store ends this thread's reservation, as below.
      BackendContextFor(context)->flags &=
          ~(uint32_t{1} << kX64BackendHasCheckedReserveBit);
      call->predicate = 0;
      return 1;
    }
  }
  const std::optional<GuestMemoryOperandType> operand_type =
      UnpackOperandType(packed_site);
  GuestMemoryAccessRequest request;
  request.instruction_address = UnpackInstructionAddress(packed_site);
  if (!operation.has_value() || !operand_type.has_value() || !call) {
    const GuestMemoryAccessResult failure = InvalidRuntimeRequest(request);
    DeliverCheckedMemoryFault(context, translation_domain, request, failure);
  }

  request.operation = *operation;
  request.operand_type = *operand_type;
  request.effective_address = call->effective_address;
  request.byte_count = call->byte_count;
  request.fill_value = call->primary_bytes[0];
  request.input_bytes = call->primary_bytes;
  std::copy(call->compare_bytes.begin(), call->compare_bytes.end(),
            request.compare_bytes.begin());
  // Structural validation requires a store-conditional to carry an identity.
  // The exact saved identity is copied below after checking the local reserve.
  request.reservation_identity_valid =
      request.operation == GuestMemoryAccessOperation::kStoreConditional;
  request.checked_tlb = checked_tlb;

  if (ValidateGuestMemoryAccessRequest(request) !=
      GuestMemoryAccessDiagnostic::kNone) {
    const GuestMemoryAccessResult failure = InvalidRuntimeRequest(request);
    DeliverCheckedMemoryFault(context, translation_domain, request, failure);
  }
  if (!context || !translation_domain) {
    const GuestMemoryAccessResult failure = InvalidRuntimeRequest(request);
    DeliverCheckedMemoryFault(context, translation_domain, request, failure);
  }

  X64BackendContext* const backend_context = BackendContextFor(context);
  constexpr uint32_t kHasReservationMask = uint32_t{1}
                                           << kX64BackendHasCheckedReserveBit;
  if (request.operation == GuestMemoryAccessOperation::kStoreConditional) {
    const bool has_exact_reservation =
        (backend_context->flags & kHasReservationMask) != 0 &&
        backend_context->checked_reserve_domain_id ==
            translation_domain->translation_domain_key().id() &&
        backend_context->checked_reserve_effective_address ==
            request.effective_address &&
        backend_context->checked_reserve_byte_count == request.byte_count;
    backend_context->flags &= ~kHasReservationMask;
    if (!has_exact_reservation) {
      call->predicate = 0;
      return 1;
    }
    std::memcpy(request.compare_bytes.data(),
                &backend_context->checked_reserve_value, request.byte_count);
    std::copy_n(backend_context->checked_reserve_identity,
                request.reservation_identity.words.size(),
                request.reservation_identity.words.begin());
    request.reservation_identity_valid = true;
  } else if (IsWriteOperation(request.operation)) {
    backend_context->flags &= ~kHasReservationMask;
  }

  const GuestMemoryAccessResult result =
      DispatchCheckedGuestMemoryAccess(translation_domain, request);
  if (!result) {
    backend_context->flags &= ~kHasReservationMask;
    if (request.operation == GuestMemoryAccessOperation::kStoreConditional &&
        result.diagnostic == GuestMemoryAccessDiagnostic::kCommitRace &&
        result.fault.reason == GuestMemoryAccessFaultReason::kCommitRace) {
      // The runtime retries a lost commit race at the same CIA, so keep the
      // reservation. Guest faults still consume it.
      backend_context->flags |= kHasReservationMask;
    }
    DeliverCheckedMemoryFault(context, translation_domain, request, result);
  }

  if (result.output_byte_count != 0) {
    std::copy_n(result.output_bytes.cbegin(), result.output_byte_count,
                call->primary_bytes.begin());
  }
  if (request.operation == GuestMemoryAccessOperation::kLoadReserve) {
    backend_context->checked_reserve_value = 0;
    std::memcpy(&backend_context->checked_reserve_value,
                result.output_bytes.data(), result.output_byte_count);
    backend_context->checked_reserve_domain_id =
        translation_domain->translation_domain_key().id();
    backend_context->checked_reserve_effective_address =
        request.effective_address;
    backend_context->checked_reserve_byte_count = request.byte_count;
    std::copy(result.reservation_identity.words.cbegin(),
              result.reservation_identity.words.cend(),
              backend_context->checked_reserve_identity);
    backend_context->flags |= kHasReservationMask;
  }
  call->predicate = result.predicate ? 1 : 0;
  return 1;
}

bool SelectCheckedGuestMemorySequence(X64Emitter* emitter,
                                      const Instr* instruction) {
  if (!emitter || !instruction || !instruction->opcode ||
      (instruction->opcode->flags & hir::OPCODE_FLAG_MEMORY) == 0) {
    return false;
  }
  X64Emitter& e = *emitter;
  switch (instruction->GetOpcodeNum()) {
    case hir::OPCODE_LOAD:
      return EmitLoadStore(e, *instruction, GuestMemoryAccessOperation::kLoad,
                           instruction->dest, instruction->src1.value, nullptr);
    case hir::OPCODE_STORE:
      return EmitLoadStore(e, *instruction, GuestMemoryAccessOperation::kStore,
                           instruction->src2.value, instruction->src1.value,
                           nullptr);
    case hir::OPCODE_LOAD_OFFSET:
      return EmitLoadStore(e, *instruction, GuestMemoryAccessOperation::kLoad,
                           instruction->dest, instruction->src1.value,
                           instruction->src2.value);
    case hir::OPCODE_STORE_OFFSET:
      return EmitLoadStore(e, *instruction, GuestMemoryAccessOperation::kStore,
                           instruction->src3.value, instruction->src1.value,
                           instruction->src2.value);
    case hir::OPCODE_LVL:
      return EmitVectorLeftRight(e, *instruction,
                                 GuestMemoryAccessOperation::kLoadVectorLeft);
    case hir::OPCODE_LVR:
      return EmitVectorLeftRight(e, *instruction,
                                 GuestMemoryAccessOperation::kLoadVectorRight);
    case hir::OPCODE_STVL:
      return EmitVectorLeftRight(e, *instruction,
                                 GuestMemoryAccessOperation::kStoreVectorLeft);
    case hir::OPCODE_STVR:
      return EmitVectorLeftRight(e, *instruction,
                                 GuestMemoryAccessOperation::kStoreVectorRight);
    case hir::OPCODE_MEMSET:
      return EmitMemset(e, *instruction);
    case hir::OPCODE_MEMORY_BARRIER:
      e.mfence();
      return true;
    case hir::OPCODE_ATOMIC_COMPARE_EXCHANGE:
      return EmitAtomicCompareExchange(e, *instruction);
    case hir::OPCODE_RESERVED_LOAD:
      return EmitReservation(e, *instruction,
                             GuestMemoryAccessOperation::kLoadReserve);
    case hir::OPCODE_RESERVED_STORE:
      return EmitReservation(e, *instruction,
                             GuestMemoryAccessOperation::kStoreConditional);
    case hir::OPCODE_LOAD_MMIO:
    case hir::OPCODE_STORE_MMIO:
      return false;
    case hir::OPCODE_CACHE_CONTROL:
      // Known guest cache hints are no-ops; unknown flags fail translation.
      return IsCheckedCacheControlNoOp(instruction->flags);
    default:
      // A memory opcode not handled above fails translation.
      return false;
  }
}

}  // namespace xe::cpu::backend::x64

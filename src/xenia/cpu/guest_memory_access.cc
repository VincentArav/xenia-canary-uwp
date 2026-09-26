/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_memory_access.h"

#include "xenia/cpu/translation_domain_registry.h"

namespace xe::cpu {
namespace {

bool IsKnownOperation(GuestMemoryAccessOperation operation) noexcept {
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
      return true;
  }
  return false;
}

bool IsKnownOperandType(GuestMemoryOperandType operand_type) noexcept {
  return GuestMemoryOperandByteCount(operand_type) != 0;
}

bool IsReadResultOperation(GuestMemoryAccessOperation operation) noexcept {
  switch (operation) {
    case GuestMemoryAccessOperation::kLoad:
    case GuestMemoryAccessOperation::kLoadVectorLeft:
    case GuestMemoryAccessOperation::kLoadVectorRight:
    case GuestMemoryAccessOperation::kLoadReserve:
      return true;
    default:
      return false;
  }
}

GuestMemoryAccessResult MakeFailure(GuestMemoryAccessDiagnostic diagnostic,
                                    const GuestMemoryAccessRequest& request) {
  GuestMemoryAccessResult result;
  result.diagnostic = diagnostic;
  result.fault.instruction_address = request.instruction_address;
  result.fault.effective_address = request.effective_address;
  result.fault.byte_count = request.byte_count;
  return result;
}

}  // namespace

GuestMemoryAccessDiagnostic ValidateGuestMemoryAccessRequest(
    const GuestMemoryAccessRequest& request) noexcept {
  if (!IsKnownOperation(request.operation) ||
      !IsKnownOperandType(request.operand_type)) {
    return GuestMemoryAccessDiagnostic::kInvalidRequest;
  }

  const uint32_t operand_byte_count =
      GuestMemoryOperandByteCount(request.operand_type);
  if (request.operation == GuestMemoryAccessOperation::kMemset) {
    return request.operand_type == GuestMemoryOperandType::kInt8 &&
                   request.byte_count != 0
               ? GuestMemoryAccessDiagnostic::kNone
               : GuestMemoryAccessDiagnostic::kInvalidRequest;
  }
  if (request.byte_count != operand_byte_count) {
    return GuestMemoryAccessDiagnostic::kInvalidRequest;
  }

  switch (request.operation) {
    case GuestMemoryAccessOperation::kLoadVectorLeft:
    case GuestMemoryAccessOperation::kLoadVectorRight:
    case GuestMemoryAccessOperation::kStoreVectorLeft:
    case GuestMemoryAccessOperation::kStoreVectorRight:
      return request.operand_type == GuestMemoryOperandType::kVector128
                 ? GuestMemoryAccessDiagnostic::kNone
                 : GuestMemoryAccessDiagnostic::kInvalidRequest;
    case GuestMemoryAccessOperation::kAtomicCompareExchange:
      return request.operand_type == GuestMemoryOperandType::kInt32 ||
                     request.operand_type == GuestMemoryOperandType::kInt64
                 ? GuestMemoryAccessDiagnostic::kNone
                 : GuestMemoryAccessDiagnostic::kInvalidRequest;
    case GuestMemoryAccessOperation::kLoadReserve:
      return (request.operand_type == GuestMemoryOperandType::kInt32 ||
              request.operand_type == GuestMemoryOperandType::kInt64) &&
                     !request.reservation_identity_valid
                 ? GuestMemoryAccessDiagnostic::kNone
                 : GuestMemoryAccessDiagnostic::kInvalidRequest;
    case GuestMemoryAccessOperation::kStoreConditional:
      return (request.operand_type == GuestMemoryOperandType::kInt32 ||
              request.operand_type == GuestMemoryOperandType::kInt64) &&
                     request.reservation_identity_valid
                 ? GuestMemoryAccessDiagnostic::kNone
                 : GuestMemoryAccessDiagnostic::kInvalidRequest;
    case GuestMemoryAccessOperation::kLoad:
    case GuestMemoryAccessOperation::kStore:
      return GuestMemoryAccessDiagnostic::kNone;
    case GuestMemoryAccessOperation::kMemset:
      break;
  }
  return GuestMemoryAccessDiagnostic::kInvalidRequest;
}

bool GuestMemoryAccessCapabilitiesSupport(
    const GuestMemoryAccessCapabilities& capabilities,
    const GuestMemoryAccessRequest& request) noexcept {
  if (ValidateGuestMemoryAccessRequest(request) !=
      GuestMemoryAccessDiagnostic::kNone) {
    return false;
  }

  const uint32_t operand_bit = GuestMemoryOperandTypeBit(request.operand_type);
  switch (request.operation) {
    case GuestMemoryAccessOperation::kLoad:
      return (capabilities.load_operand_types & operand_bit) != 0;
    case GuestMemoryAccessOperation::kStore:
      return (capabilities.store_operand_types & operand_bit) != 0;
    case GuestMemoryAccessOperation::kLoadVectorLeft:
    case GuestMemoryAccessOperation::kLoadVectorRight:
      return capabilities.vector_left_right_load;
    case GuestMemoryAccessOperation::kStoreVectorLeft:
    case GuestMemoryAccessOperation::kStoreVectorRight:
      return capabilities.vector_left_right_store;
    case GuestMemoryAccessOperation::kMemset:
      return capabilities.memset;
    case GuestMemoryAccessOperation::kAtomicCompareExchange:
      return request.operand_type == GuestMemoryOperandType::kInt32
                 ? capabilities.atomic_compare_exchange_32
                 : capabilities.atomic_compare_exchange_64;
    case GuestMemoryAccessOperation::kLoadReserve:
    case GuestMemoryAccessOperation::kStoreConditional:
      return request.operand_type == GuestMemoryOperandType::kInt32
                 ? capabilities.reservation_32
                 : capabilities.reservation_64;
  }
  return false;
}

GuestMemoryAccessResult DispatchCheckedGuestMemoryAccess(
    const TranslationDomainDescriptor* descriptor,
    const GuestMemoryAccessRequest& request) noexcept {
  const GuestMemoryAccessDiagnostic request_diagnostic =
      ValidateGuestMemoryAccessRequest(request);
  if (request_diagnostic != GuestMemoryAccessDiagnostic::kNone) {
    return MakeFailure(request_diagnostic, request);
  }
  if (!descriptor ||
      descriptor->translation_domain_key().memory_access_mode() !=
          MemoryAccessMode::kChecked) {
    return MakeFailure(GuestMemoryAccessDiagnostic::kDomainUnavailable,
                       request);
  }

  const GuestMemoryAccessor* const accessor =
      descriptor->guest_memory_accessor();
  if (!accessor ||
      accessor->memory_access_mode() != MemoryAccessMode::kChecked) {
    return MakeFailure(GuestMemoryAccessDiagnostic::kDomainUnavailable,
                       request);
  }
  if (!GuestMemoryAccessCapabilitiesSupport(accessor->capabilities(),
                                            request)) {
    return MakeFailure(GuestMemoryAccessDiagnostic::kUnsupportedRequest,
                       request);
  }

  GuestMemoryAccessResult result = accessor->Access(request);
  if (!result) {
    result.output_byte_count = 0;
    result.output_bytes = {};
    result.predicate = false;
    result.reservation_identity = {};
    result.reservation_identity_valid = false;
    return result;
  }

  const uint32_t expected_output_byte_count =
      IsReadResultOperation(request.operation) ? request.byte_count : 0;
  const bool expected_reservation_identity =
      request.operation == GuestMemoryAccessOperation::kLoadReserve;
  const bool predicate_operation =
      request.operation == GuestMemoryAccessOperation::kAtomicCompareExchange ||
      request.operation == GuestMemoryAccessOperation::kStoreConditional;
  if (result.output_byte_count != expected_output_byte_count ||
      result.reservation_identity_valid != expected_reservation_identity ||
      (!predicate_operation && result.predicate) ||
      result.fault.reason != GuestMemoryAccessFaultReason::kNone ||
      result.fault.stage != GuestMemoryAccessFaultStage::kNone) {
    return MakeFailure(GuestMemoryAccessDiagnostic::kInvalidAccessorResult,
                       request);
  }
  return result;
}

}  // namespace xe::cpu

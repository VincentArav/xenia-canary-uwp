/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_guest_data_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include "xenia/base/mutex.h"
#include "xenia/cpu/checked_tlb.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

std::optional<UserModeGuestDataOperand> ToUserModeGuestDataOperand(
    cpu::GuestMemoryOperandType operand_type) {
  switch (operand_type) {
    case cpu::GuestMemoryOperandType::kInt8:
      return UserModeGuestDataOperand::kScalar8;
    case cpu::GuestMemoryOperandType::kInt16:
      return UserModeGuestDataOperand::kScalar16;
    case cpu::GuestMemoryOperandType::kInt32:
    case cpu::GuestMemoryOperandType::kFloat32:
      return UserModeGuestDataOperand::kScalar32;
    case cpu::GuestMemoryOperandType::kInt64:
    case cpu::GuestMemoryOperandType::kFloat64:
      return UserModeGuestDataOperand::kScalar64;
    case cpu::GuestMemoryOperandType::kVector128:
      return UserModeGuestDataOperand::kVector128;
  }
  return std::nullopt;
}

cpu::GuestMemoryAccessFaultStage ToCpuFaultStage(
    UserModeGuestDataPhysicalAccessStage stage) {
  switch (stage) {
    case UserModeGuestDataPhysicalAccessStage::kRootEntry:
      return cpu::GuestMemoryAccessFaultStage::kRootEntry;
    case UserModeGuestDataPhysicalAccessStage::kPageTableEntry:
      return cpu::GuestMemoryAccessFaultStage::kPageTableEntry;
    case UserModeGuestDataPhysicalAccessStage::kOperandFragment:
      return cpu::GuestMemoryAccessFaultStage::kOperand;
    case UserModeGuestDataPhysicalAccessStage::kPteChangedBitWriteback:
      return cpu::GuestMemoryAccessFaultStage::kPteChangedBitWriteback;
    case UserModeGuestDataPhysicalAccessStage::kNone:
      return cpu::GuestMemoryAccessFaultStage::kNone;
  }
  return cpu::GuestMemoryAccessFaultStage::kNone;
}

cpu::GuestMemoryAccessResult MakeCpuAccessFailure(
    const cpu::GuestMemoryAccessRequest& request,
    UserModeGuestDataAccessDiagnostic diagnostic,
    UserModeGuestDataPhysicalAccessStage physical_stage) {
  cpu::GuestMemoryAccessResult result;
  result.fault.instruction_address = request.instruction_address;
  result.fault.effective_address = request.effective_address;
  result.fault.byte_count = request.byte_count;
  result.fault.backend_diagnostic = static_cast<uint32_t>(diagnostic);
  result.fault.stage = ToCpuFaultStage(physical_stage);
  switch (diagnostic) {
    case UserModeGuestDataAccessDiagnostic::kUnalignedEffectiveAddress:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kGuestFault;
      result.fault.reason = cpu::GuestMemoryAccessFaultReason::kAlignment;
      result.fault.stage = cpu::GuestMemoryAccessFaultStage::kRequest;
      break;
    case UserModeGuestDataAccessDiagnostic::kAddressTranslationFailed:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kGuestFault;
      result.fault.reason =
          cpu::GuestMemoryAccessFaultReason::kAddressTranslation;
      break;
    case UserModeGuestDataAccessDiagnostic::kGuestAccessFault:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kGuestFault;
      result.fault.reason = cpu::GuestMemoryAccessFaultReason::kProtection;
      break;
    case UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kBackingUnavailable;
      result.fault.reason = cpu::GuestMemoryAccessFaultReason::kPhysicalBacking;
      break;
    case UserModeGuestDataAccessDiagnostic::kMappingNotCurrent:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kMappingRetired;
      result.fault.reason = cpu::GuestMemoryAccessFaultReason::kMappingRetired;
      break;
    case UserModeGuestDataAccessDiagnostic::kStoreDisabled:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kStoreDisabled;
      break;
    case UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit:
    case UserModeGuestDataAccessDiagnostic::kPteCompareExchangeFailed:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kCommitRace;
      result.fault.reason = cpu::GuestMemoryAccessFaultReason::kCommitRace;
      result.fault.stage = cpu::GuestMemoryAccessFaultStage::kCommit;
      break;
    case UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInternalFailure;
      result.fault.stage = cpu::GuestMemoryAccessFaultStage::kInvalidation;
      break;
    case UserModeGuestDataAccessDiagnostic::kInvalidConfiguration:
    case UserModeGuestDataAccessDiagnostic::kInvalidOperand:
    case UserModeGuestDataAccessDiagnostic::kInvalidCanonicalInput:
    case UserModeGuestDataAccessDiagnostic::kUnsupportedWriteTopology:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInvalidRequest;
      break;
    case UserModeGuestDataAccessDiagnostic::kNone:
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInternalFailure;
      break;
  }
  return result;
}

std::optional<uint32_t> OperandByteCount(UserModeGuestDataOperand operand) {
  switch (operand) {
    case UserModeGuestDataOperand::kScalar8:
      return 1;
    case UserModeGuestDataOperand::kScalar16:
      return 2;
    case UserModeGuestDataOperand::kScalar32:
      return 4;
    case UserModeGuestDataOperand::kScalar64:
      return 8;
    case UserModeGuestDataOperand::kVector128:
      return 16;
    default:
      return std::nullopt;
  }
}

bool StoreModeAllowsCommit(UserModeGuestDataStoreMode store_mode) {
  return store_mode == UserModeGuestDataStoreMode::kSyntheticCommitOnly ||
         store_mode == UserModeGuestDataStoreMode::kLiveCheckedExecution;
}

UserModeGuestDataReservationIdentity MakeReservationIdentity(
    const UserModeGuestDataTransferPlan& plan,
    const UserModeAddressTranslationResult& translation) {
  return {
      plan.fragments[0].storage_effective_address, translation.physical_address,
      translation.pte_physical_address,
      translation.raw_pte & ~(kUserModePteChanged | kUserModePteReferenced)};
}

cpu::GuestMemoryReservationIdentity ToCpuReservationIdentity(
    const UserModeGuestDataReservationIdentity& identity) {
  cpu::GuestMemoryReservationIdentity result;
  result.words = {identity.storage_effective_address, identity.physical_address,
                  identity.pte_physical_address, identity.stable_raw_pte};
  return result;
}

UserModeGuestDataReservationIdentity FromCpuReservationIdentity(
    const cpu::GuestMemoryReservationIdentity& identity) {
  return {identity.words[0], identity.words[1], identity.words[2],
          identity.words[3]};
}

UserModeGuestDataTransferPlan PlanPartialVectorStore(
    UserModeGuestDataPartialVectorStore operation, uint32_t effective_address,
    bool little_endian) {
  UserModeGuestDataTransferPlan plan;
  plan.operand = UserModeGuestDataOperand::kVector128;
  plan.effective_address = effective_address;
  plan.little_endian = little_endian;
  const uint32_t aligned_address = effective_address & ~0xFu;
  const uint32_t byte_offset = effective_address & 0xFu;
  const uint32_t canonical_start =
      operation == UserModeGuestDataPartialVectorStore::kLeft ? byte_offset : 0;
  const uint32_t canonical_end =
      operation == UserModeGuestDataPartialVectorStore::kLeft ? 16
                                                              : byte_offset;
  if ((operation != UserModeGuestDataPartialVectorStore::kLeft &&
       operation != UserModeGuestDataPartialVectorStore::kRight) ||
      canonical_start == canonical_end) {
    plan.diagnostic = UserModeGuestDataTransferPlanDiagnostic::kInvalidOperand;
    return plan;
  }
  plan.canonical_byte_count = canonical_end - canonical_start;
  if (!little_endian) {
    plan.fragment_count = 1;
    plan.fragments[0] = {aligned_address + canonical_start,
                         plan.canonical_byte_count, 0};
    return plan;
  }

  const uint32_t high_end = std::min(canonical_end, uint32_t{8});
  if (canonical_start < high_end) {
    const uint32_t byte_count = high_end - canonical_start;
    plan.fragments[plan.fragment_count++] = {
        aligned_address + 8 + canonical_start, byte_count, 0};
  }
  const uint32_t low_start = std::max(canonical_start, uint32_t{8});
  if (low_start < canonical_end) {
    plan.fragments[plan.fragment_count++] = {aligned_address + low_start - 8,
                                             canonical_end - low_start,
                                             low_start - canonical_start};
  }
  return plan;
}

UserModeGuestDataAccessDiagnostic AccessDiagnosticForPlan(
    UserModeGuestDataTransferPlanDiagnostic diagnostic) {
  switch (diagnostic) {
    case UserModeGuestDataTransferPlanDiagnostic::kInvalidOperand:
      return UserModeGuestDataAccessDiagnostic::kInvalidOperand;
    case UserModeGuestDataTransferPlanDiagnostic::kUnalignedEffectiveAddress:
      return UserModeGuestDataAccessDiagnostic::kUnalignedEffectiveAddress;
    case UserModeGuestDataTransferPlanDiagnostic::kNone:
    default:
      return UserModeGuestDataAccessDiagnostic::kNone;
  }
}

UserModeGuestDataPhysicalAccessStage TranslationReadStage(
    const UserModeCreateTimeAddressSpaceLayout& layout,
    uint32_t effective_address, size_t read_index) {
  const UserModePageTableKind page_table_kind =
      layout.segment_for_effective_address(effective_address)
          .translation.page_table_kind;
  return page_table_kind != UserModePageTableKind::k16MiB && read_index == 0
             ? UserModeGuestDataPhysicalAccessStage::kRootEntry
             : UserModeGuestDataPhysicalAccessStage::kPageTableEntry;
}

bool IsPhysicalBackingTranslationFailure(
    const UserModeAddressTranslationResult& translation,
    const std::optional<UserModePhysicalReadResult>& physical_failure) {
  return translation.diagnostic ==
             UserModeAddressTranslationDiagnostic::kBackingReadFailed &&
         physical_failure.has_value();
}

void AddPteWritebackRequirement(
    const UserModeAddressTranslationResult& translation,
    std::array<UserModeGuestDataPteWritebackRequirement, 2>* requirements,
    size_t* requirement_count) {
  for (size_t i = 0; i < *requirement_count; ++i) {
    UserModeGuestDataPteWritebackRequirement& existing = (*requirements)[i];
    if (existing.pte_physical_address == translation.pte_physical_address) {
      existing.observed_raw_pte = translation.raw_pte;
      existing.writeback_raw_pte = translation.raw_pte | kUserModePteChanged;
      existing.changed_bit_writeback_required |=
          translation.changed_bit_writeback_required;
      return;
    }
  }

  UserModeGuestDataPteWritebackRequirement& requirement =
      (*requirements)[(*requirement_count)++];
  requirement.pte_physical_address = translation.pte_physical_address;
  requirement.observed_raw_pte = translation.raw_pte;
  requirement.writeback_raw_pte = translation.raw_pte | kUserModePteChanged;
  requirement.changed_bit_writeback_required =
      translation.changed_bit_writeback_required;
}

struct PreparedWrite {
  UserModeGuestDataAccessDiagnostic diagnostic =
      UserModeGuestDataAccessDiagnostic::kNone;
  std::array<std::optional<UserModeAddressTranslationResult>, 2> translations;
  std::optional<size_t> failure_fragment_index;
  UserModeGuestDataPhysicalAccessStage physical_failure_stage =
      UserModeGuestDataPhysicalAccessStage::kNone;
  std::optional<UserModePhysicalReadResult> physical_read_failure;
  std::optional<UserModePhysicalWriteProbeResult> physical_write_probe_failure;
  std::array<UserModeGuestDataPteWritebackRequirement, 2>
      pte_writeback_requirements{};
  size_t pte_writeback_requirement_count = 0;

  explicit operator bool() const {
    return diagnostic == UserModeGuestDataAccessDiagnostic::kNone;
  }
};

PreparedWrite PrepareWrite(const UserModeCreateTimeAddressSpaceLayout& layout,
                           uint32_t control_page_physical_address,
                           UserModeProblemStateKey problem_state_key,
                           const UserModeGuestDataTransferPlan& plan,
                           const UserModePhysicalReadTransaction& transaction) {
  PreparedWrite prepared;
  for (size_t fragment_index = 0; fragment_index < plan.fragment_count;
       ++fragment_index) {
    const UserModeGuestDataTransferFragment& fragment =
        plan.fragments[fragment_index];
    size_t translation_read_index = 0;
    std::optional<UserModePhysicalReadResult> translation_physical_failure;
    prepared.translations[fragment_index] = ResolveUserModeLiveAddress(
        layout, control_page_physical_address, problem_state_key,
        UserModeMemoryAccess::kWrite, fragment.storage_effective_address,
        [&transaction, &translation_physical_failure, &prepared, fragment_index,
         &layout, &translation_read_index,
         &fragment](uint32_t physical_address, std::span<uint8_t> destination) {
          const UserModePhysicalReadResult read =
              transaction.TryRead(physical_address, destination);
          if (!read) {
            translation_physical_failure = read;
            prepared.failure_fragment_index = fragment_index;
            prepared.physical_failure_stage =
                TranslationReadStage(layout, fragment.storage_effective_address,
                                     translation_read_index);
          }
          ++translation_read_index;
          return static_cast<bool>(read);
        });

    const UserModeAddressTranslationResult& translation =
        *prepared.translations[fragment_index];
    if (!translation.translated()) {
      prepared.failure_fragment_index = fragment_index;
      if (IsPhysicalBackingTranslationFailure(translation,
                                              translation_physical_failure)) {
        prepared.diagnostic =
            UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
        prepared.physical_read_failure = translation_physical_failure;
      } else {
        prepared.diagnostic =
            UserModeGuestDataAccessDiagnostic::kAddressTranslationFailed;
      }
      return prepared;
    }
    if (!translation.access_permitted()) {
      prepared.diagnostic =
          UserModeGuestDataAccessDiagnostic::kGuestAccessFault;
      prepared.failure_fragment_index = fragment_index;
      return prepared;
    }

    const UserModePhysicalWriteProbeResult physical_probe =
        transaction.ProbeWrite(translation.physical_address,
                               fragment.byte_count);
    if (!physical_probe) {
      prepared.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
      prepared.failure_fragment_index = fragment_index;
      prepared.physical_failure_stage =
          UserModeGuestDataPhysicalAccessStage::kOperandFragment;
      prepared.physical_write_probe_failure = physical_probe;
      return prepared;
    }

    if (translation.changed_bit_writeback_required) {
      const UserModePhysicalWriteProbeResult pte_write_probe =
          transaction.ProbeWrite(translation.pte_physical_address,
                                 sizeof(uint32_t));
      if (!pte_write_probe) {
        prepared.diagnostic =
            UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
        prepared.failure_fragment_index = fragment_index;
        prepared.physical_failure_stage =
            UserModeGuestDataPhysicalAccessStage::kPteChangedBitWriteback;
        prepared.physical_write_probe_failure = pte_write_probe;
        return prepared;
      }
      AddPteWritebackRequirement(translation,
                                 &prepared.pte_writeback_requirements,
                                 &prepared.pte_writeback_requirement_count);
    }
  }
  return prepared;
}

bool TranslationsHaveSameCommitIdentity(
    const UserModeAddressTranslationResult& initial,
    const UserModeAddressTranslationResult& commit) {
  return initial.diagnostic == commit.diagnostic &&
         initial.access_faults == commit.access_faults &&
         initial.requested_access == commit.requested_access &&
         initial.problem_state_key == commit.problem_state_key &&
         initial.effective_address == commit.effective_address &&
         initial.segment_index == commit.segment_index &&
         initial.root_entry_physical_address ==
             commit.root_entry_physical_address &&
         initial.pte_physical_address == commit.pte_physical_address &&
         initial.raw_pte == commit.raw_pte &&
         initial.effective_pte == commit.effective_pte &&
         initial.physical_address == commit.physical_address &&
         initial.physical_page_base == commit.physical_page_base &&
         initial.page_size == commit.page_size &&
         initial.effective_protection == commit.effective_protection &&
         initial.changed_bit_writeback_required ==
             commit.changed_bit_writeback_required;
}

bool PteWritebackRequirementsMatch(const PreparedWrite& initial,
                                   const PreparedWrite& commit) {
  if (initial.pte_writeback_requirement_count !=
      commit.pte_writeback_requirement_count) {
    return false;
  }
  for (size_t i = 0; i < initial.pte_writeback_requirement_count; ++i) {
    const UserModeGuestDataPteWritebackRequirement& initial_requirement =
        initial.pte_writeback_requirements[i];
    const UserModeGuestDataPteWritebackRequirement& commit_requirement =
        commit.pte_writeback_requirements[i];
    if (initial_requirement.pte_physical_address !=
            commit_requirement.pte_physical_address ||
        initial_requirement.observed_raw_pte !=
            commit_requirement.observed_raw_pte ||
        initial_requirement.writeback_raw_pte !=
            commit_requirement.writeback_raw_pte ||
        initial_requirement.changed_bit_writeback_required !=
            commit_requirement.changed_bit_writeback_required) {
      return false;
    }
  }
  return true;
}

struct PhysicalMutationRange {
  uint32_t physical_address = 0;
  uint32_t byte_count = 0;
};

void AddPhysicalMutationRange(uint32_t physical_address, uint32_t byte_count,
                              std::array<PhysicalMutationRange, 3>* ranges,
                              size_t* range_count) {
  for (size_t i = 0; i < *range_count; ++i) {
    if ((*ranges)[i].physical_address == physical_address &&
        (*ranges)[i].byte_count == byte_count) {
      return;
    }
  }
  (*ranges)[(*range_count)++] = {physical_address, byte_count};
}

// A thread's cached translations (cpu/checked_tlb.h): direct-mapped by
// effective 4 KiB page and tagged with the accessor's id, reads and writes
// apart, since a write translation needs write permission and a PTE whose C
// bit is already set. Callers without a context's table use this thread's.
thread_local std::unique_ptr<cpu::CheckedTlb> fallback_tlb;

std::atomic<uint32_t> next_tlb_accessor_id{1};

// Page-table words are always physical memory: the control page is below
// 0x20000000 and a leaf table ends at most at 0x1FFFFFFF, never in the image
// window.
bool TranslationSourceCurrent(const Memory& memory,
                              const cpu::CheckedTlbSource& source) {
  if (source.root_entry_physical_address &&
      xe::load_and_swap<uint16_t>(memory.TranslatePhysical<const uint8_t*>(
          source.root_entry_physical_address)) != source.root_entry) {
    return false;
  }
  return xe::load_and_swap<uint32_t>(memory.TranslatePhysical<const uint8_t*>(
             source.pte_physical_address)) == source.raw_pte;
}

void RevalidateEntries(const Memory& memory, cpu::CheckedTlbEntry* entries,
                       const cpu::CheckedTlbSource* sources) {
  for (uint32_t i = 0; i < cpu::kCheckedTlbEntryCount; ++i) {
    if (entries[i].tag && !TranslationSourceCurrent(memory, sources[i])) {
      entries[i] = {};
    }
  }
}

// The table, brought up to the epoch sampled before the lookup: after TB
// flushes alone it keeps the entries whose page-table words are unchanged;
// otherwise it empties.
cpu::CheckedTlb& AcquireTlb(cpu::CheckedTlb* tlb, const Memory& memory,
                            uint64_t epoch) {
  if (!tlb) {
    if (!fallback_tlb) {
      fallback_tlb = std::make_unique<cpu::CheckedTlb>();
      fallback_tlb->epoch = 0;
      fallback_tlb->clear_epoch = 0;
      fallback_tlb->owner = nullptr;
      fallback_tlb->stale = 0;
      fallback_tlb->ClearEntries();
    }
    tlb = fallback_tlb.get();
  }
  // Clear the flag before reading the epoch, so an advance racing with this
  // sync is still seen.
  std::atomic_ref<uint32_t> stale(tlb->stale);
  if (stale.load(std::memory_order_relaxed) || tlb->epoch != epoch) {
    stale.store(0, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    epoch = std::max(epoch, Memory::translation_epoch());
  }
  if (tlb->epoch != epoch) {
    const uint64_t clear_epoch = Memory::translation_clear_epoch();
    if (tlb->clear_epoch != clear_epoch || tlb->owner != &memory) {
      tlb->ClearEntries();
    } else {
      RevalidateEntries(memory, tlb->read, tlb->read_source);
      RevalidateEntries(memory, tlb->write, tlb->write_source);
    }
    tlb->epoch = epoch;
    tlb->clear_epoch = clear_epoch;
    tlb->owner = &memory;
  }
  return *tlb;
}

cpu::CheckedTlbSource SourceOf(const UserModeAddressTranslationResult& walk,
                               uint32_t raw_pte) {
  return {walk.root_entry_physical_address, walk.root_entry,
          walk.pte_physical_address, raw_pte};
}

const cpu::CheckedTlbEntry* LookupCachedTranslation(
    const cpu::CheckedTlbEntry* entries, uint32_t accessor_id,
    uint32_t effective_address) {
  const cpu::CheckedTlbEntry& entry =
      entries[cpu::CheckedTlb::IndexFor(effective_address)];
  if ((entry.tag & ~cpu::kCheckedTlbInlineBit) !=
          cpu::CheckedTlb::TagFor(effective_address, accessor_id) ||
      entry.accessor_id != accessor_id) {
    return nullptr;
  }
  return &entry;
}

uint8_t* CachedHostAddress(const cpu::CheckedTlbEntry& entry,
                           uint32_t effective_address) {
  return reinterpret_cast<uint8_t*>(entry.host_delta + effective_address);
}

// host_page holds the physical page's bytes: physical memory, or for a write
// the guest view page that maps it writable, which only inline_usable entries
// use. For an image window page it is always the XEX image page it names.
void InsertCachedTranslation(cpu::CheckedTlbEntry* entries,
                             cpu::CheckedTlbSource* sources,
                             uint32_t accessor_id, uint32_t effective_address,
                             uint32_t physical_address,
                             const uint8_t* host_page, bool inline_usable,
                             const cpu::CheckedTlbSource& source) {
  constexpr uint32_t kOffsetMask = (1u << cpu::kCheckedTlbPageShift) - 1;
  if ((effective_address & kOffsetMask) != (physical_address & kOffsetMask)) {
    return;
  }
  sources[cpu::CheckedTlb::IndexFor(effective_address)] = source;
  entries[cpu::CheckedTlb::IndexFor(effective_address)] = {
      cpu::CheckedTlb::TagFor(effective_address, accessor_id) |
          (inline_usable ? cpu::kCheckedTlbInlineBit : 0),
      accessor_id,
      reinterpret_cast<uint64_t>(host_page) -
          (effective_address & ~kOffsetMask)};
}

// The page of a guest physical view that maps the physical page committed
// and writable, if any. The 0xE0000000 view is 4 KiB above physical memory.
uint8_t* FindWritableAliasPage(Memory& memory, uint32_t physical_page) {
  // An image window page's only view is its XEX image page, which no physical
  // observer watches. The physical views at its address map unrelated memory.
  if (IsImagePhysicalWindowAddress(physical_page)) {
    const uint32_t image_address = ImageVirtualAddressFromWindow(physical_page);
    const BaseHeap* heap = memory.LookupHeap(image_address);
    if (!memory.image_physical_window_enabled() || !heap ||
        heap->heap_type() != HeapType::kGuestXex) {
      return nullptr;
    }
    const std::optional<HeapPageInfo> page_info =
        heap->QueryPageInfo(image_address);
    if (page_info && (page_info->state & kMemoryAllocationCommit) &&
        (page_info->protect & kMemoryProtectWrite)) {
      return memory.TranslateVirtual<uint8_t*>(image_address);
    }
    return nullptr;
  }
  constexpr std::array<std::pair<uint32_t, uint32_t>, 3> kViews = {
      {{0xA0000000u, 0u}, {0xC0000000u, 0u}, {0xE0000000u, 0x1000u}}};
  for (const auto& [view_base, physical_offset] : kViews) {
    if (physical_page < physical_offset) {
      continue;
    }
    const uint32_t alias_address = view_base + physical_page - physical_offset;
    const BaseHeap* heap = memory.LookupHeap(alias_address);
    if (!heap) {
      continue;
    }
    const std::optional<HeapPageInfo> page_info =
        heap->QueryPageInfo(alias_address);
    if (page_info && (page_info->state & kMemoryAllocationCommit) &&
        (page_info->protect & kMemoryProtectWrite)) {
      return memory.TranslateVirtual<uint8_t*>(alias_address);
    }
  }
  return nullptr;
}

}  // namespace

UserModeGuestDataTransferPlan PlanUserModeGuestDataTransfer(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    bool little_endian) {
  UserModeGuestDataTransferPlan plan;
  plan.operand = operand;
  plan.effective_address = effective_address;
  plan.little_endian = little_endian;

  const std::optional<uint32_t> byte_count = OperandByteCount(operand);
  if (!byte_count.has_value()) {
    plan.diagnostic = UserModeGuestDataTransferPlanDiagnostic::kInvalidOperand;
    return plan;
  }
  plan.canonical_byte_count = *byte_count;
  if (effective_address & (*byte_count - 1)) {
    plan.diagnostic =
        UserModeGuestDataTransferPlanDiagnostic::kUnalignedEffectiveAddress;
    return plan;
  }

  if (!little_endian) {
    plan.fragments[0] = {effective_address, *byte_count, 0};
    plan.fragment_count = 1;
    return plan;
  }

  switch (operand) {
    case UserModeGuestDataOperand::kScalar8:
      plan.fragments[0] = {effective_address ^ 7, 1, 0};
      plan.fragment_count = 1;
      break;
    case UserModeGuestDataOperand::kScalar16:
      plan.fragments[0] = {effective_address ^ 6, 2, 0};
      plan.fragment_count = 1;
      break;
    case UserModeGuestDataOperand::kScalar32:
      plan.fragments[0] = {effective_address ^ 4, 4, 0};
      plan.fragment_count = 1;
      break;
    case UserModeGuestDataOperand::kScalar64:
      plan.fragments[0] = {effective_address, 8, 0};
      plan.fragment_count = 1;
      break;
    case UserModeGuestDataOperand::kVector128:
      plan.fragments[0] = {effective_address + 8, 8, 0};
      plan.fragments[1] = {effective_address, 8, 8};
      plan.fragment_count = 2;
      break;
    default:
      plan.diagnostic =
          UserModeGuestDataTransferPlanDiagnostic::kInvalidOperand;
      break;
  }
  return plan;
}

UserModeGuestDataAccessor::UserModeGuestDataAccessor(
    Memory* memory,
    std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
    UserModeProblemStateKey problem_state_key, bool little_endian,
    UserModeGuestDataStoreMode store_mode)
    : memory_(memory),
      generation_token_(std::move(generation_token)),
      problem_state_key_(problem_state_key),
      little_endian_(little_endian),
      store_mode_(store_mode),
      tlb_accessor_id_(
          next_tlb_accessor_id.fetch_add(1, std::memory_order_relaxed)) {}

UserModeGuestDataAccessor::~UserModeGuestDataAccessor() = default;

void UserModeGuestDataAccessor::InvalidateTranslationCaches() {
  Memory::AdvanceTranslationEpochForTbFlush();
}

UserModeGuestDataReadResult UserModeGuestDataAccessor::ReadAlignedCached(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    cpu::CheckedTlb* checked_tlb) const {
  UserModeGuestDataTransferPlan plan =
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_);
  if (!plan || !memory_ || !HasValidConfiguration()) {
    return ReadAligned(operand, effective_address);
  }
  const uint64_t epoch = Memory::translation_epoch();
  if (IsMappingCurrent()) {
    cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    std::array<const uint8_t*, 2> sources{};
    bool hit = true;
    for (size_t i = 0; hit && i < plan.fragment_count; ++i) {
      const uint32_t storage_address =
          plan.fragments[i].storage_effective_address;
      const cpu::CheckedTlbEntry* entry =
          LookupCachedTranslation(tlb.read, tlb_accessor_id_, storage_address);
      hit = entry != nullptr;
      sources[i] = hit ? CachedHostAddress(*entry, storage_address) : nullptr;
    }
    if (hit) {
      std::array<uint8_t, 16> canonical_bytes{};
      for (size_t i = 0; i < plan.fragment_count; ++i) {
        const UserModeGuestDataTransferFragment& fragment = plan.fragments[i];
        std::memcpy(canonical_bytes.data() + fragment.canonical_byte_offset,
                    sources[i], fragment.byte_count);
      }
      UserModeGuestDataReadResult result;
      result.plan = plan;
      result.canonical_bytes = canonical_bytes;
      return result;
    }
  }

  UserModeGuestDataReadResult result = ReadAligned(operand, effective_address);
  // Cache only what was walked under the epoch looked up first: a TB flush
  // during the walk may have changed it. An advance completing after the check
  // syncs the table past that epoch, and the table then takes none.
  if (result && Memory::translation_epoch() == epoch) {
    cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    for (size_t i = 0; tlb.epoch == epoch && i < result.plan.fragment_count;
         ++i) {
      const uint32_t physical_address =
          result.translations[i]->physical_address;
      InsertCachedTranslation(
          tlb.read, tlb.read_source, tlb_accessor_id_,
          result.plan.fragments[i].storage_effective_address, physical_address,
          TranslateUserModePhysical(*memory_, physical_address & ~0xFFFu), true,
          SourceOf(*result.translations[i], result.translations[i]->raw_pte));
    }
  }
  return result;
}

UserModeGuestDataStoreResult UserModeGuestDataAccessor::StoreAlignedCached(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    std::span<const uint8_t> canonical_input,
    cpu::CheckedTlb* checked_tlb) const {
  UserModeGuestDataTransferPlan plan =
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_);
  if (!plan || !memory_ || !HasValidConfiguration() ||
      !StoreModeAllowsCommit(store_mode_) || !canonical_input.data() ||
      canonical_input.size() != plan.canonical_byte_count) {
    return CommitSyntheticAlignedStore(operand, effective_address,
                                       canonical_input);
  }
  const uint64_t epoch = Memory::translation_epoch();
  if (IsMappingCurrent()) {
    cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    std::array<uint8_t*, 2> destinations{};
    size_t hits = 0;
    size_t alias_hits = 0;
    for (size_t i = 0; i < plan.fragment_count; ++i) {
      const uint32_t storage_address =
          plan.fragments[i].storage_effective_address;
      const cpu::CheckedTlbEntry* entry =
          LookupCachedTranslation(tlb.write, tlb_accessor_id_, storage_address);
      if (!entry) {
        break;
      }
      ++hits;
      alias_hits += (entry->tag & cpu::kCheckedTlbInlineBit) ? 1 : 0;
      destinations[i] = CachedHostAddress(*entry, storage_address);
    }
    // The input may alias the destination.
    std::array<uint8_t, 16> canonical_bytes{};
    std::copy(canonical_input.begin(), canonical_input.end(),
              canonical_bytes.begin());
    if (hits == plan.fragment_count && alias_hits == hits) {
      // Through the guest views that map the pages writable. The mapping
      // cannot be deleted while this thread runs in it.
      for (size_t i = 0; i < plan.fragment_count; ++i) {
        const UserModeGuestDataTransferFragment& fragment = plan.fragments[i];
        std::memcpy(destinations[i],
                    canonical_bytes.data() + fragment.canonical_byte_offset,
                    fragment.byte_count);
      }
      UserModeGuestDataStoreResult result;
      result.plan = plan;
      result.write_performed = true;
      return result;
    }
    if (hits == plan.fragment_count && alias_hits == 0) {
      // Straight to physical memory. As in CommitSyntheticPlannedStore:
      // observers first, under the global lock, then the lease that mapping
      // retirement waits for. Entries without the inline bit are physical
      // memory: image window entries are always inline.
      const uint8_t* const physical_base =
          memory_->TranslatePhysical<const uint8_t*>(0);
      auto global_lock = global_critical_region::AcquireDirect();
      bool notified = true;
      for (size_t i = 0; notified && i < plan.fragment_count; ++i) {
        notified =
            memory_->NotifyPhysicalMemoryWriteInvalidationCallbacksLocked(
                global_lock,
                static_cast<uint32_t>(destinations[i] - physical_base),
                plan.fragments[i].byte_count);
      }
      UserModeMappingGenerationOperationLease operation_lease(
          generation_token_);
      // A callback may have flushed the TB or retired the mapping.
      if (notified && operation_lease && Memory::translation_epoch() == epoch) {
        for (size_t i = 0; i < plan.fragment_count; ++i) {
          const UserModeGuestDataTransferFragment& fragment = plan.fragments[i];
          std::memcpy(destinations[i],
                      canonical_bytes.data() + fragment.canonical_byte_offset,
                      fragment.byte_count);
        }
        UserModeGuestDataStoreResult result;
        result.plan = plan;
        result.invalidation_notification_count = plan.fragment_count;
        result.write_performed = true;
        return result;
      }
    }
  }

  UserModeGuestDataStoreResult result =
      CommitSyntheticAlignedStore(operand, effective_address, canonical_input);
  // The commit set the PTE's C bit if it was clear, so later stores need no
  // page-table write.
  if (result.write_performed && Memory::translation_epoch() == epoch) {
    cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    // As in ReadAlignedCached.
    for (size_t i = 0; tlb.epoch == epoch && i < result.plan.fragment_count;
         ++i) {
      const UserModeAddressTranslationResult& walk =
          *result.commit_translations[i];
      const uint32_t physical_address = walk.physical_address;
      const uint32_t physical_page = physical_address & ~0xFFFu;
      // Writing through a guest view raises the physical write observers the
      // way title code does: the page faults into them only while one
      // watches it.
      uint8_t* const alias_page =
          FindWritableAliasPage(*memory_, physical_page);
      // An image window page no longer writable is not cached: physical
      // memory at its address is unrelated.
      if (!alias_page && IsImagePhysicalWindowAddress(physical_page)) {
        continue;
      }
      // The commit set the C bit if the walk found it clear.
      InsertCachedTranslation(
          tlb.write, tlb.write_source, tlb_accessor_id_,
          result.plan.fragments[i].storage_effective_address, physical_address,
          alias_page
              ? alias_page
              : memory_->TranslatePhysical<const uint8_t*>(physical_page),
          alias_page != nullptr,
          SourceOf(walk, walk.raw_pte | (walk.changed_bit_writeback_required
                                             ? kUserModePteChanged
                                             : 0)));
    }
  }
  return result;
}

UserModeGuestDataStoreResult UserModeGuestDataAccessor::MemsetAlignedCached(
    uint32_t effective_address, uint8_t value, uint32_t byte_count,
    cpu::CheckedTlb* checked_tlb) const {
  // The commit validates the size and alignment, which keep the range within
  // one page.
  if (!memory_ || little_endian_ || !StoreModeAllowsCommit(store_mode_) ||
      (byte_count != 32 && byte_count != 128) ||
      (effective_address & (byte_count - 1)) != 0) {
    return CommitSyntheticAlignedMemset(effective_address, value, byte_count);
  }
  const uint64_t epoch = Memory::translation_epoch();
  if (IsMappingCurrent()) {
    const cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    const cpu::CheckedTlbEntry* entry =
        LookupCachedTranslation(tlb.write, tlb_accessor_id_, effective_address);
    if (entry && (entry->tag & cpu::kCheckedTlbInlineBit)) {
      std::memset(CachedHostAddress(*entry, effective_address), value,
                  byte_count);
      UserModeGuestDataStoreResult result;
      result.plan.operand = UserModeGuestDataOperand::kVector128;
      result.plan.effective_address = effective_address;
      result.plan.canonical_byte_count = byte_count;
      result.plan.fragment_count = 1;
      result.plan.fragments[0] = {effective_address, byte_count, 0};
      result.write_performed = true;
      return result;
    }
  }
  UserModeGuestDataStoreResult result =
      CommitSyntheticAlignedMemset(effective_address, value, byte_count);
  if (result.write_performed && result.commit_translations[0] &&
      Memory::translation_epoch() == epoch) {
    cpu::CheckedTlb& tlb = AcquireTlb(checked_tlb, *memory_, epoch);
    // As in ReadAlignedCached.
    if (tlb.epoch != epoch) {
      return result;
    }
    const UserModeAddressTranslationResult& walk =
        *result.commit_translations[0];
    const uint32_t physical_page = walk.physical_address & ~0xFFFu;
    uint8_t* const alias_page = FindWritableAliasPage(*memory_, physical_page);
    // As in StoreAlignedCached.
    if (!alias_page && IsImagePhysicalWindowAddress(physical_page)) {
      return result;
    }
    InsertCachedTranslation(
        tlb.write, tlb.write_source, tlb_accessor_id_, effective_address,
        walk.physical_address,
        alias_page ? alias_page
                   : memory_->TranslatePhysical<const uint8_t*>(physical_page),
        alias_page != nullptr,
        SourceOf(walk, walk.raw_pte | (walk.changed_bit_writeback_required
                                           ? kUserModePteChanged
                                           : 0)));
  }
  return result;
}

bool UserModeGuestDataAccessor::TryReadBytesCached(
    uint32_t effective_address, std::span<uint8_t> destination,
    cpu::CheckedTlb* checked_tlb) const {
  constexpr uint32_t kPageSize = 1u << cpu::kCheckedTlbPageShift;
  if (little_endian_ || destination.empty() || destination.size() > kPageSize ||
      !memory_ || !IsMappingCurrent()) {
    return false;
  }
  const cpu::CheckedTlb& tlb =
      AcquireTlb(checked_tlb, *memory_, Memory::translation_epoch());
  const uint32_t size = static_cast<uint32_t>(destination.size());
  const uint32_t first_size =
      std::min(size, kPageSize - (effective_address & (kPageSize - 1)));
  const cpu::CheckedTlbEntry* first =
      LookupCachedTranslation(tlb.read, tlb_accessor_id_, effective_address);
  const cpu::CheckedTlbEntry* second =
      first && first_size < size
          ? LookupCachedTranslation(tlb.read, tlb_accessor_id_,
                                    effective_address + first_size)
          : nullptr;
  if (!first || (first_size < size && !second)) {
    return false;
  }
  std::memcpy(destination.data(), CachedHostAddress(*first, effective_address),
              first_size);
  if (first_size < size) {
    std::memcpy(destination.data() + first_size,
                CachedHostAddress(*second, effective_address + first_size),
                size - first_size);
  }
  return true;
}

bool UserModeGuestDataAccessor::TryWriteBytesCached(
    uint32_t effective_address, std::span<const uint8_t> source,
    cpu::CheckedTlb* checked_tlb) const {
  constexpr uint32_t kPageSize = 1u << cpu::kCheckedTlbPageShift;
  if (little_endian_ || source.empty() || source.size() > kPageSize ||
      !memory_ || !StoreModeAllowsCommit(store_mode_) || !IsMappingCurrent()) {
    return false;
  }
  const cpu::CheckedTlb& tlb =
      AcquireTlb(checked_tlb, *memory_, Memory::translation_epoch());
  const uint32_t size = static_cast<uint32_t>(source.size());
  const uint32_t first_size =
      std::min(size, kPageSize - (effective_address & (kPageSize - 1)));
  const cpu::CheckedTlbEntry* first =
      LookupCachedTranslation(tlb.write, tlb_accessor_id_, effective_address);
  const cpu::CheckedTlbEntry* second =
      first && first_size < size
          ? LookupCachedTranslation(tlb.write, tlb_accessor_id_,
                                    effective_address + first_size)
          : nullptr;
  // Only writes through a watched guest view reach the physical write
  // observers without the global lock.
  if (!first || !(first->tag & cpu::kCheckedTlbInlineBit) ||
      (first_size < size &&
       (!second || !(second->tag & cpu::kCheckedTlbInlineBit)))) {
    return false;
  }
  // The source may alias the destination.
  std::array<uint8_t, 16> bytes{};
  if (size > bytes.size()) {
    return false;
  }
  std::copy(source.begin(), source.end(), bytes.begin());
  std::memcpy(CachedHostAddress(*first, effective_address), bytes.data(),
              first_size);
  if (first_size < size) {
    std::memcpy(CachedHostAddress(*second, effective_address + first_size),
                bytes.data() + first_size, size - first_size);
  }
  return true;
}

bool UserModeGuestDataAccessor::HasCachedWriteTranslation(
    uint32_t effective_address, cpu::CheckedTlb* checked_tlb) const {
  if (!memory_ || !IsMappingCurrent()) {
    return false;
  }
  const UserModeGuestDataTransferPlan plan = PlanUserModeGuestDataTransfer(
      UserModeGuestDataOperand::kScalar8, effective_address, little_endian_);
  return plan &&
         LookupCachedTranslation(
             AcquireTlb(checked_tlb, *memory_, Memory::translation_epoch())
                 .write,
             tlb_accessor_id_,
             plan.fragments[0].storage_effective_address) != nullptr;
}

bool UserModeGuestDataAccessor::HasValidConfiguration() const {
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

bool UserModeGuestDataAccessor::IsMappingCurrent() const {
  return generation_token_ && generation_token_->is_current();
}

UserModeGuestDataReadResult UserModeGuestDataAccessor::ReadAligned(
    UserModeGuestDataOperand operand, uint32_t effective_address) const {
  UserModeGuestDataReadResult result;
  if (!HasValidConfiguration()) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidConfiguration;
    return result;
  }
  result.plan =
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_);
  if (!result.plan) {
    result.diagnostic = AccessDiagnosticForPlan(result.plan.diagnostic);
    return result;
  }
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  std::array<uint8_t, 16> canonical_bytes{};
  const UserModeCreateTimeAddressSpaceLayout& layout =
      *generation_token_->create_time_layout();
  for (size_t fragment_index = 0; fragment_index < result.plan.fragment_count;
       ++fragment_index) {
    const UserModeGuestDataTransferFragment& fragment =
        result.plan.fragments[fragment_index];
    size_t translation_read_index = 0;
    std::optional<UserModePhysicalReadResult> translation_physical_failure;
    result.translations[fragment_index] = ResolveUserModeLiveAddress(
        layout, generation_token_->control_page_physical_address(),
        problem_state_key_, UserModeMemoryAccess::kRead,
        fragment.storage_effective_address,
        [&transaction, &translation_physical_failure, &result, fragment_index,
         &layout, &translation_read_index](uint32_t physical_address,
                                           std::span<uint8_t> destination) {
          const UserModePhysicalReadResult read =
              transaction.TryRead(physical_address, destination);
          if (!read) {
            translation_physical_failure = read;
            result.failure_fragment_index = fragment_index;
            result.physical_failure_stage = TranslationReadStage(
                layout,
                result.plan.fragments[fragment_index].storage_effective_address,
                translation_read_index);
          }
          ++translation_read_index;
          return static_cast<bool>(read);
        });

    if (!IsMappingCurrent()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
      return result;
    }
    const UserModeAddressTranslationResult& translation =
        *result.translations[fragment_index];
    if (!translation.translated()) {
      result.failure_fragment_index = fragment_index;
      if (IsPhysicalBackingTranslationFailure(translation,
                                              translation_physical_failure)) {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
        result.physical_read_failure = translation_physical_failure;
      } else {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kAddressTranslationFailed;
      }
      return result;
    }
    if (!translation.access_permitted()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kGuestAccessFault;
      result.failure_fragment_index = fragment_index;
      return result;
    }

    const UserModePhysicalReadResult data_read = transaction.TryRead(
        translation.physical_address,
        std::span<uint8_t>(
            canonical_bytes.data() + fragment.canonical_byte_offset,
            fragment.byte_count));
    if (!IsMappingCurrent()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
      return result;
    }
    if (!data_read) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
      result.failure_fragment_index = fragment_index;
      result.physical_failure_stage =
          UserModeGuestDataPhysicalAccessStage::kOperandFragment;
      result.physical_read_failure = data_read;
      return result;
    }
  }

  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  result.canonical_bytes = canonical_bytes;
  return result;
}

UserModeGuestDataWriteProbeResult UserModeGuestDataAccessor::ProbeAlignedWrite(
    UserModeGuestDataOperand operand, uint32_t effective_address) const {
  UserModeGuestDataWriteProbeResult result;
  if (!HasValidConfiguration()) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidConfiguration;
    return result;
  }
  result.plan =
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_);
  if (!result.plan) {
    result.diagnostic = AccessDiagnosticForPlan(result.plan.diagnostic);
    return result;
  }
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  std::array<UserModeGuestDataPteWritebackRequirement, 2>
      pte_writeback_requirements{};
  size_t pte_writeback_requirement_count = 0;
  const UserModeCreateTimeAddressSpaceLayout& layout =
      *generation_token_->create_time_layout();
  for (size_t fragment_index = 0; fragment_index < result.plan.fragment_count;
       ++fragment_index) {
    const UserModeGuestDataTransferFragment& fragment =
        result.plan.fragments[fragment_index];
    size_t translation_read_index = 0;
    std::optional<UserModePhysicalReadResult> translation_physical_failure;
    result.translations[fragment_index] = ResolveUserModeLiveAddress(
        layout, generation_token_->control_page_physical_address(),
        problem_state_key_, UserModeMemoryAccess::kWrite,
        fragment.storage_effective_address,
        [&transaction, &translation_physical_failure, &result, fragment_index,
         &layout, &translation_read_index](uint32_t physical_address,
                                           std::span<uint8_t> destination) {
          const UserModePhysicalReadResult read =
              transaction.TryRead(physical_address, destination);
          if (!read) {
            translation_physical_failure = read;
            result.failure_fragment_index = fragment_index;
            result.physical_failure_stage = TranslationReadStage(
                layout,
                result.plan.fragments[fragment_index].storage_effective_address,
                translation_read_index);
          }
          ++translation_read_index;
          return static_cast<bool>(read);
        });

    if (!IsMappingCurrent()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
      return result;
    }
    const UserModeAddressTranslationResult& translation =
        *result.translations[fragment_index];
    if (!translation.translated()) {
      result.failure_fragment_index = fragment_index;
      if (IsPhysicalBackingTranslationFailure(translation,
                                              translation_physical_failure)) {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
        result.physical_read_failure = translation_physical_failure;
      } else {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kAddressTranslationFailed;
      }
      return result;
    }
    if (!translation.access_permitted()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kGuestAccessFault;
      result.failure_fragment_index = fragment_index;
      return result;
    }

    const UserModePhysicalWriteProbeResult physical_probe =
        transaction.ProbeWrite(translation.physical_address,
                               fragment.byte_count);
    if (!IsMappingCurrent()) {
      result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
      return result;
    }
    if (!physical_probe) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
      result.failure_fragment_index = fragment_index;
      result.physical_failure_stage =
          UserModeGuestDataPhysicalAccessStage::kOperandFragment;
      result.physical_write_probe_failure = physical_probe;
      return result;
    }

    if (translation.changed_bit_writeback_required) {
      const UserModePhysicalWriteProbeResult pte_write_probe =
          transaction.ProbeWrite(translation.pte_physical_address,
                                 sizeof(uint32_t));
      if (!IsMappingCurrent()) {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
        return result;
      }
      if (!pte_write_probe) {
        result.diagnostic =
            UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
        result.failure_fragment_index = fragment_index;
        result.physical_failure_stage =
            UserModeGuestDataPhysicalAccessStage::kPteChangedBitWriteback;
        result.physical_write_probe_failure = pte_write_probe;
        return result;
      }
      AddPteWritebackRequirement(translation, &pte_writeback_requirements,
                                 &pte_writeback_requirement_count);
    }
  }

  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  result.pte_writeback_requirements = pte_writeback_requirements;
  result.pte_writeback_requirement_count = pte_writeback_requirement_count;
  return result;
}

UserModeGuestDataStoreResult
UserModeGuestDataAccessor::CommitSyntheticAlignedStore(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    std::span<const uint8_t> canonical_input) const {
  return CommitSyntheticPlannedStore(
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_),
      canonical_input);
}

UserModeGuestDataStoreResult
UserModeGuestDataAccessor::CommitSyntheticPartialVectorStore(
    UserModeGuestDataPartialVectorStore operation, uint32_t effective_address,
    std::span<const uint8_t> architectural_vector_input) const {
  UserModeGuestDataTransferPlan plan =
      PlanPartialVectorStore(operation, effective_address, little_endian_);
  if (!architectural_vector_input.data() ||
      architectural_vector_input.size() != 16 || !plan) {
    return CommitSyntheticPlannedStore(std::move(plan),
                                       std::span<const uint8_t>{});
  }
  std::array<uint8_t, 16> selected_bytes{};
  const uint32_t byte_offset = effective_address & 0xFu;
  const uint32_t source_offset =
      operation == UserModeGuestDataPartialVectorStore::kLeft
          ? 0
          : 16 - byte_offset;
  std::copy_n(architectural_vector_input.begin() + source_offset,
              plan.canonical_byte_count, selected_bytes.begin());
  const uint32_t selected_byte_count = plan.canonical_byte_count;
  return CommitSyntheticPlannedStore(
      std::move(plan),
      std::span<const uint8_t>(selected_bytes.data(), selected_byte_count));
}

UserModeGuestDataStoreResult
UserModeGuestDataAccessor::CommitSyntheticPlannedStore(
    UserModeGuestDataTransferPlan plan,
    std::span<const uint8_t> canonical_input) const {
  UserModeGuestDataStoreResult result;
  if (!HasValidConfiguration()) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidConfiguration;
    return result;
  }
  if (!StoreModeAllowsCommit(store_mode_)) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kStoreDisabled;
    return result;
  }
  result.plan = std::move(plan);
  if (!result.plan) {
    result.diagnostic = AccessDiagnosticForPlan(result.plan.diagnostic);
    return result;
  }
  if (!canonical_input.data() ||
      canonical_input.size() != result.plan.canonical_byte_count) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidCanonicalInput;
    return result;
  }

  // Copy the input first: it may alias the destination.
  std::array<uint8_t, 16> canonical_bytes{};
  std::copy(canonical_input.begin(), canonical_input.end(),
            canonical_bytes.begin());
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  const UserModeCreateTimeAddressSpaceLayout& layout =
      *generation_token_->create_time_layout();
  result.stage = UserModeGuestDataStoreStage::kInitialPreflight;
  const PreparedWrite initial =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.initial_translations = initial.translations;
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  if (!initial) {
    result.diagnostic = initial.diagnostic;
    result.failure_fragment_index = initial.failure_fragment_index;
    result.physical_failure_stage = initial.physical_failure_stage;
    result.physical_read_failure = initial.physical_read_failure;
    result.physical_write_probe_failure = initial.physical_write_probe_failure;
    return result;
  }

  // Every supported access lies within one guest page.
  uint32_t unique_pte_physical_address =
      initial.translations[0]->pte_physical_address;
  for (size_t i = 1; i < result.plan.fragment_count; ++i) {
    if (initial.translations[i]->pte_physical_address !=
        unique_pte_physical_address) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kUnsupportedWriteTopology;
      result.failure_fragment_index = i;
      return result;
    }
  }
  if (initial.pte_writeback_requirement_count > 1) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kUnsupportedWriteTopology;
    return result;
  }

  // Notify the physical write observers under the global lock but before
  // taking the operation lease: a callback may delete the mapping.
  std::array<PhysicalMutationRange, 3> mutation_ranges{};
  size_t mutation_range_count = 0;
  if (initial.pte_writeback_requirement_count != 0) {
    AddPhysicalMutationRange(
        initial.pte_writeback_requirements[0].pte_physical_address,
        sizeof(uint32_t), &mutation_ranges, &mutation_range_count);
  }
  for (size_t i = 0; i < result.plan.fragment_count; ++i) {
    AddPhysicalMutationRange(initial.translations[i]->physical_address,
                             result.plan.fragments[i].byte_count,
                             &mutation_ranges, &mutation_range_count);
  }
  result.stage = UserModeGuestDataStoreStage::kInvalidationNotification;
  for (size_t i = 0; i < mutation_range_count; ++i) {
    if (!transaction.NotifyWriteInvalidationCallbacks(
            mutation_ranges[i].physical_address,
            mutation_ranges[i].byte_count)) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed;
      return result;
    }
    ++result.invalidation_notification_count;
  }

  result.stage = UserModeGuestDataStoreStage::kOperationLease;
  UserModeMappingGenerationOperationLease operation_lease(generation_token_);
  if (!operation_lease) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  // No callbacks or manager calls while the operation lease is held;
  // retirement waits for it.
  result.stage = UserModeGuestDataStoreStage::kCommitPreflight;
  const PreparedWrite commit =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.commit_translations = commit.translations;
  if (!commit) {
    result.diagnostic = commit.diagnostic;
    result.failure_fragment_index = commit.failure_fragment_index;
    result.physical_failure_stage = commit.physical_failure_stage;
    result.physical_read_failure = commit.physical_read_failure;
    result.physical_write_probe_failure = commit.physical_write_probe_failure;
    return result;
  }
  for (size_t i = 0; i < result.plan.fragment_count; ++i) {
    if (!TranslationsHaveSameCommitIdentity(*initial.translations[i],
                                            *commit.translations[i])) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
      result.failure_fragment_index = i;
      return result;
    }
  }
  if (!PteWritebackRequirementsMatch(initial, commit)) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
    return result;
  }

  if (commit.pte_writeback_requirement_count != 0) {
    const UserModeGuestDataPteWritebackRequirement& requirement =
        commit.pte_writeback_requirements[0];
    result.stage = UserModeGuestDataStoreStage::kPteChangedBitCompareExchange;
    uint32_t observed_raw_pte = 0;
    if (!transaction.CompareExchangeBe32Prevalidated(
            requirement.pte_physical_address, requirement.observed_raw_pte,
            requirement.writeback_raw_pte, &observed_raw_pte)) {
      result.pte_compare_exchange_observed_raw = observed_raw_pte;
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPteCompareExchangeFailed;
      return result;
    }
    result.pte_compare_exchange_observed_raw = observed_raw_pte;
    result.changed_bit_writeback_performed = true;
  }

  // Nothing below can fail. The transfer plan encodes little-endian munging
  // and the vector half swap; the copies keep canonical byte order.
  result.stage = UserModeGuestDataStoreStage::kOperandCopy;
  for (size_t i = 0; i < result.plan.fragment_count; ++i) {
    const UserModeGuestDataTransferFragment& fragment =
        result.plan.fragments[i];
    transaction.CopyWritePrevalidated(
        commit.translations[i]->physical_address,
        std::span<const uint8_t>(
            canonical_bytes.data() + fragment.canonical_byte_offset,
            fragment.byte_count));
  }
  result.write_performed = true;
  result.stage = UserModeGuestDataStoreStage::kNone;
  return result;
}

UserModeGuestDataStoreResult
UserModeGuestDataAccessor::CommitSyntheticAlignedMemset(
    uint32_t effective_address, uint8_t value, uint32_t byte_count) const {
  UserModeGuestDataStoreResult result;
  if (!HasValidConfiguration()) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidConfiguration;
    return result;
  }
  if (!StoreModeAllowsCommit(store_mode_)) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kStoreDisabled;
    return result;
  }
  // dcbz clears 128 bytes, or 32 in the short-line form.
  if ((byte_count != 32 && byte_count != 128) ||
      (effective_address & (byte_count - 1)) != 0) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kUnalignedEffectiveAddress;
    return result;
  }

  result.plan.operand = UserModeGuestDataOperand::kVector128;
  result.plan.effective_address = effective_address;
  result.plan.canonical_byte_count = byte_count;
  result.plan.fragment_count = 1;
  result.plan.fragments[0] = {effective_address, byte_count, 0};
  result.plan.little_endian = little_endian_;
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  std::array<uint8_t, 128> fill_bytes;
  fill_bytes.fill(value);
  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  const UserModeCreateTimeAddressSpaceLayout& layout =
      *generation_token_->create_time_layout();
  result.stage = UserModeGuestDataStoreStage::kInitialPreflight;
  const PreparedWrite initial =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.initial_translations = initial.translations;
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  if (!initial) {
    result.diagnostic = initial.diagnostic;
    result.failure_fragment_index = initial.failure_fragment_index;
    result.physical_failure_stage = initial.physical_failure_stage;
    result.physical_read_failure = initial.physical_read_failure;
    result.physical_write_probe_failure = initial.physical_write_probe_failure;
    return result;
  }
  if (initial.pte_writeback_requirement_count > 1) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kUnsupportedWriteTopology;
    return result;
  }

  result.stage = UserModeGuestDataStoreStage::kInvalidationNotification;
  if (initial.pte_writeback_requirement_count != 0) {
    const UserModeGuestDataPteWritebackRequirement& requirement =
        initial.pte_writeback_requirements[0];
    if (!transaction.NotifyWriteInvalidationCallbacks(
            requirement.pte_physical_address, sizeof(uint32_t))) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed;
      return result;
    }
    ++result.invalidation_notification_count;
  }
  if (!transaction.NotifyWriteInvalidationCallbacks(
          initial.translations[0]->physical_address, byte_count)) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed;
    return result;
  }
  ++result.invalidation_notification_count;

  result.stage = UserModeGuestDataStoreStage::kOperationLease;
  UserModeMappingGenerationOperationLease operation_lease(generation_token_);
  if (!operation_lease) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  result.stage = UserModeGuestDataStoreStage::kCommitPreflight;
  const PreparedWrite commit =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.commit_translations = commit.translations;
  if (!commit) {
    result.diagnostic = commit.diagnostic;
    result.failure_fragment_index = commit.failure_fragment_index;
    result.physical_failure_stage = commit.physical_failure_stage;
    result.physical_read_failure = commit.physical_read_failure;
    result.physical_write_probe_failure = commit.physical_write_probe_failure;
    return result;
  }
  if (!TranslationsHaveSameCommitIdentity(*initial.translations[0],
                                          *commit.translations[0]) ||
      !PteWritebackRequirementsMatch(initial, commit)) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
    return result;
  }

  if (commit.pte_writeback_requirement_count != 0) {
    const UserModeGuestDataPteWritebackRequirement& requirement =
        commit.pte_writeback_requirements[0];
    result.stage = UserModeGuestDataStoreStage::kPteChangedBitCompareExchange;
    uint32_t observed_raw_pte = 0;
    if (!transaction.CompareExchangeBe32Prevalidated(
            requirement.pte_physical_address, requirement.observed_raw_pte,
            requirement.writeback_raw_pte, &observed_raw_pte)) {
      result.pte_compare_exchange_observed_raw = observed_raw_pte;
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPteCompareExchangeFailed;
      return result;
    }
    result.pte_compare_exchange_observed_raw = observed_raw_pte;
    result.changed_bit_writeback_performed = true;
  }

  result.stage = UserModeGuestDataStoreStage::kOperandCopy;
  transaction.CopyWritePrevalidated(
      commit.translations[0]->physical_address,
      std::span<const uint8_t>(fill_bytes.data(), byte_count));
  result.write_performed = true;
  result.stage = UserModeGuestDataStoreStage::kNone;
  return result;
}

UserModeGuestDataStoreResult
UserModeGuestDataAccessor::CompareExchangeSyntheticAligned(
    UserModeGuestDataOperand operand, uint32_t effective_address,
    std::span<const uint8_t> compare_bytes,
    std::span<const uint8_t> replacement_bytes,
    const UserModeGuestDataReservationIdentity* required_reservation_identity)
    const {
  UserModeGuestDataStoreResult result;
  if (!HasValidConfiguration()) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidConfiguration;
    return result;
  }
  if (!StoreModeAllowsCommit(store_mode_)) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kStoreDisabled;
    return result;
  }
  if (operand != UserModeGuestDataOperand::kScalar32 &&
      operand != UserModeGuestDataOperand::kScalar64) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kInvalidOperand;
    return result;
  }
  result.plan =
      PlanUserModeGuestDataTransfer(operand, effective_address, little_endian_);
  if (!result.plan) {
    result.diagnostic = AccessDiagnosticForPlan(result.plan.diagnostic);
    return result;
  }
  if (!compare_bytes.data() || !replacement_bytes.data() ||
      compare_bytes.size() != result.plan.canonical_byte_count ||
      replacement_bytes.size() != result.plan.canonical_byte_count) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidCanonicalInput;
    return result;
  }

  std::array<uint8_t, 8> expected{};
  std::array<uint8_t, 8> desired{};
  std::copy(compare_bytes.begin(), compare_bytes.end(), expected.begin());
  std::copy(replacement_bytes.begin(), replacement_bytes.end(),
            desired.begin());
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  UserModePhysicalReadTransaction transaction(*memory_);
  if (!IsMappingCurrent()) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }
  const UserModeCreateTimeAddressSpaceLayout& layout =
      *generation_token_->create_time_layout();
  result.stage = UserModeGuestDataStoreStage::kInitialPreflight;
  const PreparedWrite initial =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.initial_translations = initial.translations;
  if (!initial) {
    result.diagnostic = initial.diagnostic;
    result.failure_fragment_index = initial.failure_fragment_index;
    result.physical_failure_stage = initial.physical_failure_stage;
    result.physical_read_failure = initial.physical_read_failure;
    result.physical_write_probe_failure = initial.physical_write_probe_failure;
    return result;
  }
  if (result.plan.fragment_count != 1 ||
      initial.pte_writeback_requirement_count > 1) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kUnsupportedWriteTopology;
    return result;
  }
  if (required_reservation_identity &&
      MakeReservationIdentity(result.plan, *initial.translations[0]) !=
          *required_reservation_identity) {
    result.atomic_predicate = false;
    result.stage = UserModeGuestDataStoreStage::kNone;
    return result;
  }

  std::array<uint8_t, 8> observed{};
  UserModePhysicalReadResult observed_read = transaction.TryRead(
      initial.translations[0]->physical_address,
      std::span<uint8_t>(observed.data(), result.plan.canonical_byte_count));
  if (!observed_read) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
    result.failure_fragment_index = 0;
    result.physical_failure_stage =
        UserModeGuestDataPhysicalAccessStage::kOperandFragment;
    result.physical_read_failure = observed_read;
    return result;
  }
  if (!std::equal(expected.cbegin(),
                  expected.cbegin() + result.plan.canonical_byte_count,
                  observed.cbegin())) {
    result.atomic_predicate = false;
    result.stage = UserModeGuestDataStoreStage::kNone;
    return result;
  }

  result.stage = UserModeGuestDataStoreStage::kInvalidationNotification;
  if (initial.pte_writeback_requirement_count != 0) {
    const UserModeGuestDataPteWritebackRequirement& requirement =
        initial.pte_writeback_requirements[0];
    if (!transaction.NotifyWriteInvalidationCallbacks(
            requirement.pte_physical_address, sizeof(uint32_t))) {
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed;
      return result;
    }
    ++result.invalidation_notification_count;
  }
  if (!transaction.NotifyWriteInvalidationCallbacks(
          initial.translations[0]->physical_address,
          result.plan.canonical_byte_count)) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kInvalidationNotificationFailed;
    return result;
  }
  ++result.invalidation_notification_count;

  result.stage = UserModeGuestDataStoreStage::kOperationLease;
  UserModeMappingGenerationOperationLease operation_lease(generation_token_);
  if (!operation_lease) {
    result.diagnostic = UserModeGuestDataAccessDiagnostic::kMappingNotCurrent;
    return result;
  }

  result.stage = UserModeGuestDataStoreStage::kCommitPreflight;
  const PreparedWrite commit =
      PrepareWrite(layout, generation_token_->control_page_physical_address(),
                   problem_state_key_, result.plan, transaction);
  result.commit_translations = commit.translations;
  if (!commit) {
    result.diagnostic = commit.diagnostic;
    result.failure_fragment_index = commit.failure_fragment_index;
    result.physical_failure_stage = commit.physical_failure_stage;
    result.physical_read_failure = commit.physical_read_failure;
    result.physical_write_probe_failure = commit.physical_write_probe_failure;
    return result;
  }
  if (!TranslationsHaveSameCommitIdentity(*initial.translations[0],
                                          *commit.translations[0]) ||
      !PteWritebackRequirementsMatch(initial, commit)) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
    return result;
  }
  observed.fill(0);
  observed_read = transaction.TryRead(
      commit.translations[0]->physical_address,
      std::span<uint8_t>(observed.data(), result.plan.canonical_byte_count));
  if (!observed_read) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kPhysicalBackingFailed;
    result.failure_fragment_index = 0;
    result.physical_failure_stage =
        UserModeGuestDataPhysicalAccessStage::kOperandFragment;
    result.physical_read_failure = observed_read;
    return result;
  }
  if (!std::equal(expected.cbegin(),
                  expected.cbegin() + result.plan.canonical_byte_count,
                  observed.cbegin())) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
    return result;
  }

  if (commit.pte_writeback_requirement_count != 0) {
    const UserModeGuestDataPteWritebackRequirement& requirement =
        commit.pte_writeback_requirements[0];
    result.stage = UserModeGuestDataStoreStage::kPteChangedBitCompareExchange;
    uint32_t observed_raw_pte = 0;
    if (!transaction.CompareExchangeBe32Prevalidated(
            requirement.pte_physical_address, requirement.observed_raw_pte,
            requirement.writeback_raw_pte, &observed_raw_pte)) {
      result.pte_compare_exchange_observed_raw = observed_raw_pte;
      result.diagnostic =
          UserModeGuestDataAccessDiagnostic::kPteCompareExchangeFailed;
      return result;
    }
    result.pte_compare_exchange_observed_raw = observed_raw_pte;
    result.changed_bit_writeback_performed = true;
  }

  result.stage = UserModeGuestDataStoreStage::kOperandCopy;
  bool exchanged = false;
  if (result.plan.canonical_byte_count == sizeof(uint32_t)) {
    uint32_t expected_value = 0;
    uint32_t desired_value = 0;
    uint32_t observed_value = 0;
    std::memcpy(&expected_value, expected.data(), sizeof(expected_value));
    std::memcpy(&desired_value, desired.data(), sizeof(desired_value));
    exchanged = transaction.CompareExchangeRaw32Prevalidated(
        commit.translations[0]->physical_address, expected_value, desired_value,
        &observed_value);
  } else {
    uint64_t expected_value = 0;
    uint64_t desired_value = 0;
    uint64_t observed_value = 0;
    std::memcpy(&expected_value, expected.data(), sizeof(expected_value));
    std::memcpy(&desired_value, desired.data(), sizeof(desired_value));
    exchanged = transaction.CompareExchangeRaw64Prevalidated(
        commit.translations[0]->physical_address, expected_value, desired_value,
        &observed_value);
  }
  if (!exchanged) {
    result.diagnostic =
        UserModeGuestDataAccessDiagnostic::kTranslationChangedDuringCommit;
    return result;
  }
  result.atomic_predicate = true;
  result.write_performed = true;
  result.stage = UserModeGuestDataStoreStage::kNone;
  return result;
}

namespace {

class UserModeCpuGuestMemoryAccessor final : public cpu::GuestMemoryAccessor {
 public:
  UserModeCpuGuestMemoryAccessor(
      Memory* memory,
      std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
      UserModeProblemStateKey problem_state_key, bool little_endian,
      UserModeGuestDataStoreMode store_mode)
      : accessor_(memory, std::move(generation_token), problem_state_key,
                  little_endian, store_mode) {}

  cpu::MemoryAccessMode memory_access_mode() const noexcept override {
    return cpu::MemoryAccessMode::kChecked;
  }

  cpu::GuestMemoryAccessCapabilities capabilities() const noexcept override {
    cpu::GuestMemoryAccessCapabilities capabilities;
    capabilities.live_checked_execution =
        accessor_.store_mode() ==
        UserModeGuestDataStoreMode::kLiveCheckedExecution;
    capabilities.load_operand_types =
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt8) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt16) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt32) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kInt64) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kFloat32) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kFloat64) |
        cpu::GuestMemoryOperandTypeBit(cpu::GuestMemoryOperandType::kVector128);
    if (StoreModeAllowsCommit(accessor_.store_mode())) {
      capabilities.store_operand_types = capabilities.load_operand_types;
      capabilities.vector_left_right_store = true;
      capabilities.memset = true;
      capabilities.atomic_compare_exchange_32 = true;
      capabilities.atomic_compare_exchange_64 = true;
      capabilities.reservation_32 = true;
      capabilities.reservation_64 = true;
    }
    capabilities.vector_left_right_load = true;
    return capabilities;
  }

  bool TryFastLoad(uint32_t effective_address,
                   std::span<uint8_t> canonical_bytes,
                   cpu::CheckedTlb* checked_tlb) const noexcept override {
    // Vectors must be aligned; scalars may be misaligned.
    return (canonical_bytes.size() <= 8 ||
            (effective_address & (canonical_bytes.size() - 1)) == 0) &&
           accessor_.TryReadBytesCached(effective_address, canonical_bytes,
                                        checked_tlb);
  }

  bool TryFastStore(uint32_t effective_address,
                    std::span<const uint8_t> canonical_bytes,
                    cpu::CheckedTlb* checked_tlb) const noexcept override {
    return (canonical_bytes.size() <= 8 ||
            (effective_address & (canonical_bytes.size() - 1)) == 0) &&
           accessor_.TryWriteBytesCached(effective_address, canonical_bytes,
                                         checked_tlb);
  }

  // Big-endian storage is the canonical order, so a cached translation is
  // usable by generated code as it is.
  uint32_t inline_tlb_accessor_id() const noexcept override {
    const uint32_t id = accessor_.tlb_accessor_id();
    return accessor_.little_endian() || id > cpu::kCheckedTlbMaxPackedAccessorId
               ? 0
               : id;
  }

  cpu::GuestMemoryAccessResult Access(
      const cpu::GuestMemoryAccessRequest& request) const noexcept override {
    const std::optional<UserModeGuestDataOperand> operand =
        ToUserModeGuestDataOperand(request.operand_type);
    if (!operand.has_value() ||
        cpu::ValidateGuestMemoryAccessRequest(request) !=
            cpu::GuestMemoryAccessDiagnostic::kNone) {
      cpu::GuestMemoryAccessResult result;
      result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInvalidRequest;
      result.fault.instruction_address = request.instruction_address;
      result.fault.effective_address = request.effective_address;
      result.fault.byte_count = request.byte_count;
      return result;
    }

    // Most loads and stores hit this thread's cached translations. Vectors must
    // be aligned; scalars may be misaligned (see below).
    if ((request.byte_count <= 8 ||
         (request.effective_address & (request.byte_count - 1)) == 0) &&
        !accessor_.little_endian()) {
      if (request.operation == cpu::GuestMemoryAccessOperation::kLoad) {
        cpu::GuestMemoryAccessResult result;
        if (accessor_.TryReadBytesCached(
                request.effective_address,
                std::span<uint8_t>(result.output_bytes.data(),
                                   request.byte_count),
                request.checked_tlb)) {
          result.output_byte_count = request.byte_count;
          return result;
        }
      } else if (request.operation == cpu::GuestMemoryAccessOperation::kStore &&
                 accessor_.TryWriteBytesCached(
                     request.effective_address,
                     std::span<const uint8_t>(request.input_bytes.data(),
                                              request.byte_count),
                     request.checked_tlb)) {
        return {};
      }
    }

    // The hardware performs misaligned big-endian loads and stores; do them
    // byte by byte, so each byte translates on its own and a page-crossing
    // access works. Little-endian, reserved and atomic accesses still raise
    // alignment faults.
    if ((request.operation == cpu::GuestMemoryAccessOperation::kLoad ||
         request.operation == cpu::GuestMemoryAccessOperation::kStore) &&
        !accessor_.little_endian() && request.byte_count > 1 &&
        request.byte_count <= 8 &&
        (request.effective_address & (request.byte_count - 1)) != 0) {
      return AccessMisalignedBigEndian(request);
    }

    switch (request.operation) {
      case cpu::GuestMemoryAccessOperation::kLoad:
      case cpu::GuestMemoryAccessOperation::kLoadReserve: {
        // A reservation needs the walked translation.
        const UserModeGuestDataReadResult read =
            request.operation == cpu::GuestMemoryAccessOperation::kLoad
                ? accessor_.ReadAlignedCached(
                      *operand, request.effective_address, request.checked_tlb)
                : accessor_.ReadAligned(*operand, request.effective_address);
        if (!read) {
          return MakeCpuAccessFailure(request, read.diagnostic,
                                      read.physical_failure_stage);
        }
        cpu::GuestMemoryAccessResult result;
        result.output_byte_count = read.plan.canonical_byte_count;
        std::copy_n(read.canonical_bytes->cbegin(), result.output_byte_count,
                    result.output_bytes.begin());
        if (request.operation ==
            cpu::GuestMemoryAccessOperation::kLoadReserve) {
          result.reservation_identity = ToCpuReservationIdentity(
              MakeReservationIdentity(read.plan, *read.translations[0]));
          result.reservation_identity_valid = true;
        }
        return result;
      }
      case cpu::GuestMemoryAccessOperation::kStore: {
        const UserModeGuestDataStoreResult store = accessor_.StoreAlignedCached(
            *operand, request.effective_address,
            std::span<const uint8_t>(request.input_bytes.data(),
                                     request.byte_count),
            request.checked_tlb);
        if (!store) {
          return MakeCpuAccessFailure(request, store.diagnostic,
                                      store.physical_failure_stage);
        }
        return {};
      }
      case cpu::GuestMemoryAccessOperation::kLoadVectorLeft:
      case cpu::GuestMemoryAccessOperation::kLoadVectorRight: {
        const uint32_t aligned_address = request.effective_address & ~0xFu;
        const uint32_t byte_offset = request.effective_address & 0xFu;
        if (request.operation ==
                cpu::GuestMemoryAccessOperation::kLoadVectorRight &&
            byte_offset == 0) {
          if (!accessor_.is_mapping_current()) {
            return MakeCpuAccessFailure(
                request, UserModeGuestDataAccessDiagnostic::kMappingNotCurrent,
                UserModeGuestDataPhysicalAccessStage::kNone);
          }
          cpu::GuestMemoryAccessResult result;
          result.output_byte_count = 16;
          if (!accessor_.is_mapping_current()) {
            return MakeCpuAccessFailure(
                request, UserModeGuestDataAccessDiagnostic::kMappingNotCurrent,
                UserModeGuestDataPhysicalAccessStage::kNone);
          }
          return result;
        }
        const UserModeGuestDataReadResult read =
            accessor_.ReadAlignedCached(UserModeGuestDataOperand::kVector128,
                                        aligned_address, request.checked_tlb);
        if (!read) {
          return MakeCpuAccessFailure(request, read.diagnostic,
                                      read.physical_failure_stage);
        }
        std::array<uint8_t, 16> logical_result{};
        if (request.operation ==
            cpu::GuestMemoryAccessOperation::kLoadVectorLeft) {
          for (uint32_t i = byte_offset; i < 16; ++i) {
            logical_result[i - byte_offset] = (*read.canonical_bytes)[i];
          }
        } else {
          for (uint32_t i = 0; i < byte_offset; ++i) {
            logical_result[16 - byte_offset + i] = (*read.canonical_bytes)[i];
          }
        }

        cpu::GuestMemoryAccessResult result;
        result.output_byte_count = 16;
        result.output_bytes = logical_result;
        return result;
      }
      case cpu::GuestMemoryAccessOperation::kStoreVectorLeft:
      case cpu::GuestMemoryAccessOperation::kStoreVectorRight: {
        const uint32_t byte_offset = request.effective_address & 0xFu;
        if (request.operation ==
                cpu::GuestMemoryAccessOperation::kStoreVectorRight &&
            byte_offset == 0) {
          if (!StoreModeAllowsCommit(accessor_.store_mode())) {
            return MakeCpuAccessFailure(
                request, UserModeGuestDataAccessDiagnostic::kStoreDisabled,
                UserModeGuestDataPhysicalAccessStage::kNone);
          }
          if (!accessor_.is_mapping_current()) {
            return MakeCpuAccessFailure(
                request, UserModeGuestDataAccessDiagnostic::kMappingNotCurrent,
                UserModeGuestDataPhysicalAccessStage::kNone);
          }
          return {};
        }
        const UserModeGuestDataStoreResult store =
            accessor_.CommitSyntheticPartialVectorStore(
                request.operation ==
                        cpu::GuestMemoryAccessOperation::kStoreVectorLeft
                    ? UserModeGuestDataPartialVectorStore::kLeft
                    : UserModeGuestDataPartialVectorStore::kRight,
                request.effective_address, request.input_bytes);
        if (!store) {
          return MakeCpuAccessFailure(request, store.diagnostic,
                                      store.physical_failure_stage);
        }
        return {};
      }
      case cpu::GuestMemoryAccessOperation::kMemset: {
        const UserModeGuestDataStoreResult store =
            accessor_.MemsetAlignedCached(
                request.effective_address, request.fill_value,
                request.byte_count, request.checked_tlb);
        if (!store) {
          return MakeCpuAccessFailure(request, store.diagnostic,
                                      store.physical_failure_stage);
        }
        return {};
      }
      case cpu::GuestMemoryAccessOperation::kAtomicCompareExchange: {
        const UserModeGuestDataStoreResult compare_exchange =
            accessor_.CompareExchangeSyntheticAligned(
                *operand, request.effective_address,
                std::span<const uint8_t>(request.compare_bytes.data(),
                                         request.byte_count),
                std::span<const uint8_t>(request.input_bytes.data(),
                                         request.byte_count));
        if (!compare_exchange) {
          return MakeCpuAccessFailure(request, compare_exchange.diagnostic,
                                      compare_exchange.physical_failure_stage);
        }
        cpu::GuestMemoryAccessResult result;
        result.predicate = *compare_exchange.atomic_predicate;
        return result;
      }
      case cpu::GuestMemoryAccessOperation::kStoreConditional: {
        if (!request.reservation_identity_valid) {
          cpu::GuestMemoryAccessResult result;
          result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInvalidRequest;
          return result;
        }
        const UserModeGuestDataReservationIdentity reservation_identity =
            FromCpuReservationIdentity(request.reservation_identity);
        const UserModeGuestDataStoreResult store_conditional =
            accessor_.CompareExchangeSyntheticAligned(
                *operand, request.effective_address,
                std::span<const uint8_t>(request.compare_bytes.data(),
                                         request.byte_count),
                std::span<const uint8_t>(request.input_bytes.data(),
                                         request.byte_count),
                &reservation_identity);
        if (!store_conditional) {
          return MakeCpuAccessFailure(request, store_conditional.diagnostic,
                                      store_conditional.physical_failure_stage);
        }
        cpu::GuestMemoryAccessResult result;
        result.predicate = *store_conditional.atomic_predicate;
        return result;
      }
    }
    cpu::GuestMemoryAccessResult result;
    result.diagnostic = cpu::GuestMemoryAccessDiagnostic::kInvalidRequest;
    return result;
  }

 private:
  // Big-endian storage order is the canonical order, so byte i of the operand
  // is the byte at effective_address + i.
  cpu::GuestMemoryAccessResult AccessMisalignedBigEndian(
      const cpu::GuestMemoryAccessRequest& request) const noexcept {
    if (request.operation == cpu::GuestMemoryAccessOperation::kLoad) {
      cpu::GuestMemoryAccessResult result;
      for (uint32_t i = 0; i < request.byte_count; ++i) {
        const UserModeGuestDataReadResult read = accessor_.ReadAlignedCached(
            UserModeGuestDataOperand::kScalar8, request.effective_address + i,
            request.checked_tlb);
        if (!read) {
          return FailAtByte(request, i, read.diagnostic,
                            read.physical_failure_stage);
        }
        result.output_bytes[i] = (*read.canonical_bytes)[0];
      }
      result.output_byte_count = request.byte_count;
      return result;
    }
    // Probe every byte before storing any, so a fault on the second page does
    // not leave the first page partially written.
    for (uint32_t i = 0; i < request.byte_count; ++i) {
      if (accessor_.HasCachedWriteTranslation(request.effective_address + i,
                                              request.checked_tlb)) {
        continue;
      }
      const UserModeGuestDataWriteProbeResult probe =
          accessor_.ProbeAlignedWrite(UserModeGuestDataOperand::kScalar8,
                                      request.effective_address + i);
      if (probe.diagnostic != UserModeGuestDataAccessDiagnostic::kNone) {
        return FailAtByte(request, i, probe.diagnostic,
                          probe.physical_failure_stage);
      }
    }
    for (uint32_t i = 0; i < request.byte_count; ++i) {
      const UserModeGuestDataStoreResult store = accessor_.StoreAlignedCached(
          UserModeGuestDataOperand::kScalar8, request.effective_address + i,
          std::span<const uint8_t>(request.input_bytes.data() + i, 1),
          request.checked_tlb);
      if (!store) {
        return FailAtByte(request, i, store.diagnostic,
                          store.physical_failure_stage);
      }
    }
    return {};
  }

  // A bytewise access reports the byte that faulted, so a guest that maps
  // pages on demand maps the second page of a page-crossing access.
  static cpu::GuestMemoryAccessResult FailAtByte(
      const cpu::GuestMemoryAccessRequest& request, uint32_t byte_index,
      UserModeGuestDataAccessDiagnostic diagnostic,
      UserModeGuestDataPhysicalAccessStage physical_stage) noexcept {
    cpu::GuestMemoryAccessResult result =
        MakeCpuAccessFailure(request, diagnostic, physical_stage);
    result.fault.effective_address = request.effective_address + byte_index;
    return result;
  }

  const UserModeGuestDataAccessor accessor_;
};

}  // namespace

std::shared_ptr<const cpu::GuestMemoryAccessor>
CreateUserModeGuestMemoryAccessor(
    Memory* memory,
    std::shared_ptr<const UserModeMappingGenerationToken> generation_token,
    UserModeProblemStateKey problem_state_key, bool little_endian,
    UserModeGuestDataStoreMode store_mode) {
  if (!memory || !generation_token || !generation_token->is_current() ||
      !generation_token->handle() ||
      !generation_token->create_time_layout().has_value()) {
    return nullptr;
  }
  switch (problem_state_key) {
    case UserModeProblemStateKey::kZero:
    case UserModeProblemStateKey::kOne:
      break;
    default:
      return nullptr;
  }
  switch (store_mode) {
    case UserModeGuestDataStoreMode::kDisabled:
    case UserModeGuestDataStoreMode::kSyntheticCommitOnly:
    case UserModeGuestDataStoreMode::kLiveCheckedExecution:
      break;
    default:
      return nullptr;
  }
  return std::make_shared<const UserModeCpuGuestMemoryAccessor>(
      memory, std::move(generation_token), problem_state_key, little_endian,
      store_mode);
}

}  // namespace xe::kernel

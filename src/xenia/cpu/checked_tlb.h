/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_CHECKED_TLB_H_
#define XENIA_CPU_CHECKED_TLB_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xe {
namespace cpu {

// A thread's cached checked-domain translations. Generated code probes the
// table of the context it runs on; on a miss the host accessor fills it. Only
// the owning thread uses a table. Advancing Memory::translation_epoch()
// invalidates every table: generated code misses on a table whose epoch
// differs, and the host first revalidates it, keeping the entries whose
// page-table words are unchanged after TB flushes alone and emptying it
// otherwise.
constexpr uint32_t kCheckedTlbPageShift = 12;
constexpr uint32_t kCheckedTlbEntryCount = 4096;
// IndexFor folds the page bits above the index into it.
constexpr uint32_t kCheckedTlbIndexBits = 12;
static_assert(kCheckedTlbEntryCount == 1u << kCheckedTlbIndexBits);
// Entry tag bits above the 20-bit effective page number.
constexpr uint32_t kCheckedTlbValidBit = 0x40000000u;
// Generated code may access host_delta + ea directly. Set on every read entry
// and on write entries that go through a watched guest view, or through the
// XEX image page of an image window page, which no observer watches.
constexpr uint32_t kCheckedTlbInlineBit = 0x80000000u;
// Accessor ids up to kCheckedTlbMaxPackedAccessorId sit in tag bits 20-29, so
// generated code matches page and accessor with one compare. Entries of larger
// ids carry 0 there and serve the host only.
constexpr uint32_t kCheckedTlbAccessorShift = 20;
constexpr uint32_t kCheckedTlbMaxPackedAccessorId = 0x3FF;

struct CheckedTlbEntry {
  // CheckedTlb::TagFor(ea, accessor_id) [| kCheckedTlbInlineBit]; 0 is empty.
  uint32_t tag;
  // GuestMemoryAccessor::inline_tlb_accessor_id() of the accessor that
  // translated the page. Ids are never reused.
  uint32_t accessor_id;
  // The host address of guest byte ea is host_delta + ea (zero-extended).
  uint64_t host_delta;
};
static_assert(sizeof(CheckedTlbEntry) == 16);

// The page-table words an entry was translated from, for the host only.
struct CheckedTlbSource {
  // 0 when the translation read no root entry.
  uint32_t root_entry_physical_address;
  uint32_t root_entry;
  uint32_t pte_physical_address;
  // The PTE as it is after the access that filled the entry.
  uint32_t raw_pte;
};
static_assert(sizeof(CheckedTlbSource) == 16);

struct alignas(64) CheckedTlb {
  // Memory::translation_epoch() and translation_clear_epoch() the entries are
  // current for.
  uint64_t epoch;
  uint64_t clear_epoch;
  // The memory the entries translate into.
  const void* owner;
  // Set by every translation epoch advance (Memory::MarkTranslationTablesStale)
  // while the table is registered; generated code misses while it is set.
  uint32_t stale;
  uint8_t padding[36];
  CheckedTlbEntry read[kCheckedTlbEntryCount];
  CheckedTlbEntry write[kCheckedTlbEntryCount];
  CheckedTlbSource read_source[kCheckedTlbEntryCount];
  CheckedTlbSource write_source[kCheckedTlbEntryCount];

  void ClearEntries() {
    std::memset(read, 0, sizeof(read));
    std::memset(write, 0, sizeof(write));
  }

  // The page folded with its upper bits, so pages sharing their low bits
  // don't evict each other. Adjacent pages never share an index, so an access
  // whose tag comes from its last byte and index from its first can't match
  // across a page boundary.
  static constexpr uint32_t IndexFor(uint32_t effective_address) {
    const uint32_t page = effective_address >> kCheckedTlbPageShift;
    return (page + (page >> kCheckedTlbIndexBits)) &
           (kCheckedTlbEntryCount - 1);
  }
  static constexpr uint32_t TagFor(uint32_t effective_address,
                                   uint32_t accessor_id) {
    return (effective_address >> kCheckedTlbPageShift) | kCheckedTlbValidBit |
           (accessor_id <= kCheckedTlbMaxPackedAccessorId
                ? accessor_id << kCheckedTlbAccessorShift
                : 0);
  }
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_CHECKED_TLB_H_

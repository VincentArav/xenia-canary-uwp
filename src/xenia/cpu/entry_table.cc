/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/entry_table.h"

#include <mutex>
#include <shared_mutex>

#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"

namespace xe {
namespace cpu {

EntryTable::EntryTable() = default;

EntryTable::~EntryTable() = default;

Entry* EntryTable::Get(const TranslationAddress& address) {
  std::shared_lock<std::shared_mutex> table_lock(mutex_);
  uint32_t idx = map_.IndexForKey(address);
  if (idx == map_.size() || *map_.KeyAt(idx) != address) {
    return nullptr;
  }
  Entry* entry = *map_.ValueAt(idx);
  if (entry) {
    // TODO(benvanik): wait if needed?
    if (entry->status != Entry::STATUS_READY) {
      entry = nullptr;
    }
  }
  return entry;
}

Entry::Status EntryTable::GetOrCreate(const TranslationAddress& address,
                                      Entry** out_entry) {
  // TODO(benvanik): replace with a map with wait-free for find.
  // https://github.com/facebook/folly/blob/master/folly/AtomicHashMap.h

  std::unique_lock<std::shared_mutex> table_lock(mutex_);

  uint32_t idx = map_.IndexForKey(address);

  Entry* entry = idx != map_.size() && *map_.KeyAt(idx) == address
                     ? *map_.ValueAt(idx)
                     : nullptr;
  Entry::Status status;
  if (entry) {
    // If we aren't ready yet spin and wait.
    if (entry->status == Entry::STATUS_COMPILING) {
      // chrispy: i think this is dead code, if we are compiling we're holding
      // the global lock, arent we? so we wouldnt be executing here
      // Still compiling, so spin.
      do {
        table_lock.unlock();
        // TODO(benvanik): sleep for less time?
        xe::threading::Sleep(std::chrono::microseconds(10));
        table_lock.lock();
      } while (entry->status == Entry::STATUS_COMPILING);
    }
    status = entry->status;
  } else {
    // Create and return for initialization.
    auto owned_entry = std::make_unique<Entry>();
    entry = owned_entry.get();
    entry->translation_address = address;
    entry->end_address = 0;
    entry->status = Entry::STATUS_COMPILING;
    entry->function = 0;
    map_.InsertAt(address, entry, idx);
    entries_.push_back(std::move(owned_entry));
    // map_[address] = entry;
    status = Entry::STATUS_NEW;
  }
  table_lock.unlock();
  *out_entry = entry;
  return status;
}

void EntryTable::Delete(const TranslationAddress& address) {
  std::unique_lock<std::shared_mutex> table_lock(mutex_);
  uint32_t idx = map_.IndexForKey(address);
  if (idx != map_.size() && *map_.KeyAt(idx) == address) {
    map_.EraseAt(idx);
  }
}

std::vector<Function*> EntryTable::FindWithAddress(
    const TranslationAddress& address) {
  std::shared_lock<std::shared_mutex> table_lock(mutex_);
  std::vector<Function*> fns;
  for (auto& it : map_.Values()) {
    Entry* entry = it;
    if (entry->status.load(std::memory_order_acquire) != Entry::STATUS_READY) {
      continue;
    }
    if (address.domain_key() == entry->translation_address.domain_key() &&
        address.guest_address() >= entry->translation_address.guest_address() &&
        address.guest_address() <= entry->end_address) {
      fns.push_back(entry->function);
    }
  }
  return fns;
}

bool EntryTable::HasInProgressCompilation(TranslationDomainKey domain_key) {
  std::shared_lock<std::shared_mutex> table_lock(mutex_);
  for (Entry* entry : map_.Values()) {
    if (entry->translation_address.domain_key() == domain_key &&
        entry->status.load(std::memory_order_acquire) ==
            Entry::STATUS_COMPILING) {
      return true;
    }
  }
  return false;
}

bool EntryTable::InvalidateAddresses(TranslationDomainKey domain_key,
                                     std::span<const uint32_t> addresses,
                                     size_t* invalidated_count_out) {
  if (!invalidated_count_out) {
    return false;
  }
  *invalidated_count_out = 0;

  std::unique_lock<std::shared_mutex> table_lock(mutex_);

  // Refuse the whole request if any entry is compiling: its compiler must not
  // publish into a tombstone.
  for (uint32_t guest_address : addresses) {
    const TranslationAddress address(domain_key, guest_address);
    const uint32_t index = map_.IndexForKey(address);
    if (index != map_.size() && *map_.KeyAt(index) == address &&
        (*map_.ValueAt(index))->status.load(std::memory_order_acquire) ==
            Entry::STATUS_COMPILING) {
      return false;
    }
  }

  for (uint32_t guest_address : addresses) {
    const TranslationAddress address(domain_key, guest_address);
    const uint32_t index = map_.IndexForKey(address);
    if (index == map_.size() || *map_.KeyAt(index) != address) {
      continue;
    }
    Entry* entry = *map_.ValueAt(index);
    entry->status.store(Entry::STATUS_INVALIDATED, std::memory_order_release);
    map_.EraseAt(index);
    ++*invalidated_count_out;
  }
  return true;
}

size_t EntryTable::InvalidateDomain(TranslationDomainKey domain_key) {
  std::unique_lock<std::shared_mutex> table_lock(mutex_);
  for (Entry* entry : map_.Values()) {
    if (entry->translation_address.domain_key() == domain_key &&
        entry->status.load(std::memory_order_acquire) ==
            Entry::STATUS_COMPILING) {
      return 0;
    }
  }

  size_t invalidated_count = 0;
  for (uint32_t i = map_.size(); i != 0; --i) {
    const uint32_t index = i - 1;
    Entry* entry = *map_.ValueAt(index);
    if (entry->translation_address.domain_key() != domain_key) {
      continue;
    }
    entry->status.store(Entry::STATUS_INVALIDATED, std::memory_order_release);
    map_.EraseAt(index);
    ++invalidated_count;
  }
  return invalidated_count;
}

size_t EntryTable::InvalidateFailedEntries(TranslationDomainKey domain_key) {
  std::unique_lock<std::shared_mutex> table_lock(mutex_);
  size_t invalidated_count = 0;
  for (uint32_t i = map_.size(); i != 0; --i) {
    const uint32_t index = i - 1;
    Entry* entry = *map_.ValueAt(index);
    if (entry->translation_address.domain_key() != domain_key ||
        entry->status.load(std::memory_order_acquire) != Entry::STATUS_FAILED) {
      continue;
    }
    entry->status.store(Entry::STATUS_INVALIDATED, std::memory_order_release);
    map_.EraseAt(index);
    ++invalidated_count;
  }
  return invalidated_count;
}

Entry* EntryTable::Get(uint32_t address) {
  return Get(
      TranslationAddress(TranslationDomainKey::DefaultDirect(), address));
}

Entry::Status EntryTable::GetOrCreate(uint32_t address, Entry** out_entry) {
  return GetOrCreate(
      TranslationAddress(TranslationDomainKey::DefaultDirect(), address),
      out_entry);
}

void EntryTable::Delete(uint32_t address) {
  Delete(TranslationAddress(TranslationDomainKey::DefaultDirect(), address));
}

std::vector<Function*> EntryTable::FindWithAddress(uint32_t address) {
  return FindWithAddress(
      TranslationAddress(TranslationDomainKey::DefaultDirect(), address));
}
}  // namespace cpu
}  // namespace xe

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_ENTRY_TABLE_H_
#define XENIA_CPU_ENTRY_TABLE_H_

#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/base/split_map.h"
#include "xenia/cpu/translation_domain.h"
namespace xe {
namespace cpu {

class Function;

typedef struct Entry_t {
  typedef enum {
    STATUS_NEW = 0,
    STATUS_COMPILING,
    STATUS_READY,
    STATUS_FAILED,
    STATUS_INVALIDATED,
  } Status;

  TranslationAddress translation_address;
  uint32_t end_address;
  // Publishing READY/FAILED also publishes the function and range written by
  // the compiling thread to readers waiting on this entry.
  std::atomic<Status> status;
  Function* function;
} Entry;

class EntryTable {
 public:
  EntryTable();
  ~EntryTable();

  Entry* Get(const TranslationAddress& address);
  Entry::Status GetOrCreate(const TranslationAddress& address,
                            Entry** out_entry);
  void Delete(const TranslationAddress& address);
  std::vector<Function*> FindWithAddress(const TranslationAddress& address);
  bool HasInProgressCompilation(TranslationDomainKey domain_key);
  bool InvalidateAddresses(TranslationDomainKey domain_key,
                           std::span<const uint32_t> addresses,
                           size_t* invalidated_count_out);
  size_t InvalidateDomain(TranslationDomainKey domain_key);
  // Tombstones the domain's failed entries, so rewritten code may be retried.
  size_t InvalidateFailedEntries(TranslationDomainKey domain_key);

  Entry* Get(TranslationDomainKey domain_key, uint32_t address) {
    return Get(TranslationAddress(domain_key, address));
  }
  Entry::Status GetOrCreate(TranslationDomainKey domain_key, uint32_t address,
                            Entry** out_entry) {
    return GetOrCreate(TranslationAddress(domain_key, address), out_entry);
  }
  void Delete(TranslationDomainKey domain_key, uint32_t address) {
    Delete(TranslationAddress(domain_key, address));
  }
  std::vector<Function*> FindWithAddress(TranslationDomainKey domain_key,
                                         uint32_t address) {
    return FindWithAddress(TranslationAddress(domain_key, address));
  }

  // These use the DefaultDirect domain.
  Entry* Get(uint32_t address);
  Entry::Status GetOrCreate(uint32_t address, Entry** out_entry);
  void Delete(uint32_t address);
  std::vector<Function*> FindWithAddress(uint32_t address);

 private:
  xe::global_critical_region global_critical_region_;
  // TODO(benvanik): replace with a better data structure.
  xe::split_map<TranslationAddress, Entry*> map_;
  // Entry pointers escape the table lock while compilation is in progress.
  // Retain erased nodes until table teardown so deletion cannot race a waiter.
  std::vector<std::unique_ptr<Entry>> entries_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_ENTRY_TABLE_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_TRANSLATION_DOMAIN_REGISTRY_H_
#define XENIA_CPU_TRANSLATION_DOMAIN_REGISTRY_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "xenia/cpu/translation_domain.h"

namespace xe::cpu {

class GuestMemoryAccessor;
class Processor;

namespace ppc {
class GuestCodeReader;
}  // namespace ppc

class TranslationDomainRegistry;

// A registered domain's key, reader and memory accessor. The registry keeps
// every descriptor, retired ones included, for its lifetime.
class TranslationDomainDescriptor final {
 public:
  ~TranslationDomainDescriptor();

  const TranslationDomainKey& translation_domain_key() const {
    return translation_domain_key_;
  }
  const ppc::GuestCodeReader* guest_code_reader() const {
    return guest_code_reader_.get();
  }
  const GuestMemoryAccessor* guest_memory_accessor() const {
    return guest_memory_accessor_.get();
  }

 private:
  friend class TranslationDomainRegistry;

  TranslationDomainDescriptor(
      TranslationDomainKey translation_domain_key,
      std::unique_ptr<const ppc::GuestCodeReader> guest_code_reader,
      std::shared_ptr<const GuestMemoryAccessor> guest_memory_accessor);

  const TranslationDomainKey translation_domain_key_;
  const std::unique_ptr<const ppc::GuestCodeReader> guest_code_reader_;
  const std::shared_ptr<const GuestMemoryAccessor> guest_memory_accessor_;
};

// Owns the descriptors of the domains other than DefaultDirect. Retiring a
// domain hides it from lookup without freeing its descriptor or reusing its
// ID.
class TranslationDomainRegistry final {
 public:
  explicit TranslationDomainRegistry(Processor* processor = nullptr);
  ~TranslationDomainRegistry();

  TranslationDomainRegistry(const TranslationDomainRegistry&) = delete;
  TranslationDomainRegistry& operator=(const TranslationDomainRegistry&) =
      delete;

  // The owning Processor, or null for a standalone registry.
  Processor* processor() const { return processor_; }

  // Registers a new domain with its reader, which must report the requested
  // mode. Returns null for an unsupported or mismatched mode, a null reader or
  // exhausted IDs. The registry owns the reader; what the reader borrows must
  // outlive the registry.
  const TranslationDomainDescriptor* Register(
      MemoryAccessMode memory_access_mode,
      std::unique_ptr<const ppc::GuestCodeReader> guest_code_reader,
      std::shared_ptr<const GuestMemoryAccessor> guest_memory_accessor =
          nullptr);

  // Returns an immutable descriptor whose pointer remains valid for this
  // registry's lifetime. Retired and DefaultDirect domains are unavailable.
  const TranslationDomainDescriptor* Lookup(
      TranslationDomainKey translation_domain_key) const;

  // Makes a registered domain unavailable without releasing its storage.
  // DefaultDirect, invalid, unknown, and already-retired keys return false.
  bool Retire(TranslationDomainKey translation_domain_key);

  // Returns a synchronized snapshot. Registration or retirement may change
  // the count immediately after this call returns.
  size_t active_domain_count() const;

 private:
  Processor* const processor_;
  // Lookups run on every checked call and transition, so they share it.
  mutable std::shared_mutex mutex_;
  std::vector<std::unique_ptr<const TranslationDomainDescriptor>>
      retained_domains_;
  std::unordered_map<TranslationDomainKey, const TranslationDomainDescriptor*>
      active_domains_;
  TranslationDomainKey::Id next_domain_id_ = 1;
  bool id_space_exhausted_ = false;
};

}  // namespace xe::cpu

#endif  // XENIA_CPU_TRANSLATION_DOMAIN_REGISTRY_H_

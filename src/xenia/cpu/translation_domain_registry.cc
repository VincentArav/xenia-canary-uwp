/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/translation_domain_registry.h"

#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "xenia/cpu/guest_memory_access.h"
#include "xenia/cpu/ppc/guest_code_reader.h"
#include "xenia/cpu/processor.h"

namespace xe::cpu {
namespace {

bool IsKnownMemoryAccessMode(MemoryAccessMode memory_access_mode) {
  switch (memory_access_mode) {
    case MemoryAccessMode::kDirect:
    case MemoryAccessMode::kChecked:
      return true;
  }
  return false;
}

}  // namespace

TranslationDomainDescriptor::TranslationDomainDescriptor(
    TranslationDomainKey translation_domain_key,
    std::unique_ptr<const ppc::GuestCodeReader> guest_code_reader,
    std::shared_ptr<const GuestMemoryAccessor> guest_memory_accessor)
    : translation_domain_key_(translation_domain_key),
      guest_code_reader_(std::move(guest_code_reader)),
      guest_memory_accessor_(std::move(guest_memory_accessor)) {}

TranslationDomainDescriptor::~TranslationDomainDescriptor() = default;

TranslationDomainRegistry::TranslationDomainRegistry(Processor* processor)
    : processor_(processor) {}

TranslationDomainRegistry::~TranslationDomainRegistry() = default;

const TranslationDomainDescriptor* TranslationDomainRegistry::Register(
    MemoryAccessMode memory_access_mode,
    std::unique_ptr<const ppc::GuestCodeReader> guest_code_reader,
    std::shared_ptr<const GuestMemoryAccessor> guest_memory_accessor) {
  if (!IsKnownMemoryAccessMode(memory_access_mode) || !guest_code_reader ||
      guest_code_reader->memory_access_mode() != memory_access_mode ||
      (memory_access_mode == MemoryAccessMode::kChecked
           ? !guest_memory_accessor ||
                 guest_memory_accessor->memory_access_mode() !=
                     MemoryAccessMode::kChecked
           : static_cast<bool>(guest_memory_accessor))) {
    return nullptr;
  }

  std::lock_guard<std::shared_mutex> lock(mutex_);
  if (id_space_exhausted_ || next_domain_id_ == 0) {
    id_space_exhausted_ = true;
    return nullptr;
  }

  const TranslationDomainKey::Id domain_id = next_domain_id_;
  if (domain_id == std::numeric_limits<TranslationDomainKey::Id>::max()) {
    id_space_exhausted_ = true;
  } else {
    ++next_domain_id_;
  }

  const TranslationDomainKey translation_domain_key(domain_id,
                                                    memory_access_mode);
  auto descriptor = std::unique_ptr<const TranslationDomainDescriptor>(
      new TranslationDomainDescriptor(translation_domain_key,
                                      std::move(guest_code_reader),
                                      std::move(guest_memory_accessor)));
  const TranslationDomainDescriptor* descriptor_ptr = descriptor.get();

  // Keep the descriptor before publishing it; a consumed ID is never reused,
  // even if the insertion throws.
  retained_domains_.push_back(std::move(descriptor));
  active_domains_.emplace(translation_domain_key, descriptor_ptr);
  return descriptor_ptr;
}

const TranslationDomainDescriptor* TranslationDomainRegistry::Lookup(
    TranslationDomainKey translation_domain_key) const {
  if (translation_domain_key.id() == TranslationDomainKey::kDefaultDirectId ||
      !IsKnownMemoryAccessMode(translation_domain_key.memory_access_mode())) {
    return nullptr;
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = active_domains_.find(translation_domain_key);
  return it == active_domains_.end() ? nullptr : it->second;
}

bool TranslationDomainRegistry::Retire(
    TranslationDomainKey translation_domain_key) {
  if (translation_domain_key.id() == TranslationDomainKey::kDefaultDirectId ||
      !IsKnownMemoryAccessMode(translation_domain_key.memory_access_mode())) {
    return false;
  }

  bool retired;
  {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    retired = active_domains_.erase(translation_domain_key) != 0;
  }
  if (processor_) {
    processor_->AdvanceCheckedExecutionEpoch();
  }
  return retired;
}

size_t TranslationDomainRegistry::active_domain_count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return active_domains_.size();
}

}  // namespace xe::cpu

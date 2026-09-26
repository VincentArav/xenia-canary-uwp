/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_CODE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "xenia/cpu/function.h"
#include "xenia/cpu/translation_domain.h"

namespace xe {
namespace cpu {
namespace backend {

// How generated code dispatches guest calls. The indirection table is indexed
// by guest address alone, so only DefaultDirect uses it and direct links;
// other domains resolve every call.
enum class GuestCallDispatchMode {
  kDirectLinkAndGlobalIndirection,
  kResolverOnly,
};

constexpr GuestCallDispatchMode GetGuestCallDispatchMode(
    TranslationDomainKey translation_domain_key) {
  return translation_domain_key == TranslationDomainKey::DefaultDirect()
             ? GuestCallDispatchMode::kDirectLinkAndGlobalIndirection
             : GuestCallDispatchMode::kResolverOnly;
}

class CodeCache {
 public:
  CodeCache() = default;
  virtual ~CodeCache() = default;

  virtual const std::filesystem::path& file_name() const = 0;
  virtual uintptr_t execute_base_address() const = 0;
  virtual size_t total_size() const = 0;

  // Finds a function based on the given host PC (that may be within a
  // function).
  virtual GuestFunction* LookupFunction(uint64_t host_pc) = 0;

  // Finds platform-specific function unwind info for the given host PC.
  virtual void* LookupUnwindInfo(uint64_t host_pc) = 0;

  // Points the indirection slots of the given entry addresses back at the
  // resolver. Generated code stays resident. Fails for domains other than
  // DefaultDirect.
  virtual bool ResetGuestIndirections(
      TranslationDomainKey translation_domain_key,
      std::span<const uint32_t> guest_addresses) = 0;
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_CODE_CACHE_H_

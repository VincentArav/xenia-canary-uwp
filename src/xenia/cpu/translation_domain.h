/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_TRANSLATION_DOMAIN_H_
#define XENIA_CPU_TRANSLATION_DOMAIN_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace xe::cpu {

// How generated code accesses guest memory: directly, or checked through a
// user-mode mapping.
enum class MemoryAccessMode : uint8_t {
  kDirect = 0,
  kChecked = 1,
};

// Identity of a translation domain. ID 0 is DefaultDirect; the
// TranslationDomainRegistry mints the others and never reuses them.
class TranslationDomainKey {
 public:
  using Id = uint64_t;

  static constexpr Id kDefaultDirectId = 0;

  constexpr TranslationDomainKey() noexcept = default;
  constexpr explicit TranslationDomainKey(
      Id id,
      MemoryAccessMode memory_access_mode = MemoryAccessMode::kDirect) noexcept
      : id_(id), memory_access_mode_(memory_access_mode) {}

  static constexpr TranslationDomainKey DefaultDirect() noexcept {
    return TranslationDomainKey();
  }

  constexpr Id id() const noexcept { return id_; }
  constexpr MemoryAccessMode memory_access_mode() const noexcept {
    return memory_access_mode_;
  }

  constexpr auto operator<=>(const TranslationDomainKey&) const = default;

  size_t hash() const noexcept {
    size_t seed = std::hash<Id>{}(id_);
    const size_t mode_hash =
        std::hash<uint8_t>{}(static_cast<uint8_t>(memory_access_mode_));
    return seed ^ (mode_hash + size_t{0x9E3779B9} + (seed << 6) + (seed >> 2));
  }

 private:
  Id id_ = kDefaultDirectId;
  MemoryAccessMode memory_access_mode_ = MemoryAccessMode::kDirect;
};

// A guest address in a translation domain; functions and entries are keyed by
// it.
class TranslationAddress {
 public:
  constexpr TranslationAddress() noexcept = default;
  constexpr TranslationAddress(TranslationDomainKey domain_key,
                               uint32_t guest_address) noexcept
      : domain_key_(domain_key), guest_address_(guest_address) {}

  constexpr const TranslationDomainKey& domain_key() const noexcept {
    return domain_key_;
  }
  constexpr uint32_t guest_address() const noexcept { return guest_address_; }

  constexpr auto operator<=>(const TranslationAddress&) const = default;

  size_t hash() const noexcept {
    size_t seed = domain_key_.hash();
    const size_t address_hash = std::hash<uint32_t>{}(guest_address_);
    return seed ^
           (address_hash + size_t{0x9E3779B9} + (seed << 6) + (seed >> 2));
  }

 private:
  TranslationDomainKey domain_key_ = TranslationDomainKey::DefaultDirect();
  uint32_t guest_address_ = 0;
};

}  // namespace xe::cpu

namespace std {

template <>
struct hash<xe::cpu::TranslationDomainKey> {
  size_t operator()(const xe::cpu::TranslationDomainKey& key) const noexcept {
    return key.hash();
  }
};

template <>
struct hash<xe::cpu::TranslationAddress> {
  size_t operator()(const xe::cpu::TranslationAddress& address) const noexcept {
    return address.hash();
  }
};

}  // namespace std

#endif  // XENIA_CPU_TRANSLATION_DOMAIN_H_

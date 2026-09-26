/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_MAPPED_MODULE_H_
#define XENIA_CPU_MAPPED_MODULE_H_

#include <string>

#include "xenia/cpu/module.h"

namespace xe::cpu {

// The module of a checked domain: every address is guest code read through
// the domain's own reader. It has no flat memory range and no externs.
class MappedModule final : public Module {
 public:
  MappedModule(Processor* processor,
               TranslationDomainKey translation_domain_key, std::string name);
  ~MappedModule() override;

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  bool ContainsAddress(uint32_t address) override;

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;
  // Mapped code is rewritten live (JIT output), so a replacement scans afresh.
  bool preserves_declaration_metadata_on_invalidation() const override {
    return false;
  }

 private:
  std::string name_;
};

}  // namespace xe::cpu

#endif  // XENIA_CPU_MAPPED_MODULE_H_

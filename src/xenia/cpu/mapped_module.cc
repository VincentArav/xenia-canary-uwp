/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/mapped_module.h"

#include <utility>

#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"

namespace xe::cpu {

MappedModule::MappedModule(Processor* processor,
                           TranslationDomainKey translation_domain_key,
                           std::string name)
    : Module(processor, translation_domain_key), name_(std::move(name)) {}

MappedModule::~MappedModule() = default;

bool MappedModule::ContainsAddress(uint32_t address) {
  (void)address;
  return true;
}

std::unique_ptr<Function> MappedModule::CreateFunction(uint32_t address) {
  if (!processor_->backend()) {
    return nullptr;
  }
  // Every address, even one equal to an import thunk, is translated from this
  // domain's own bytes: an extern would reach memory through the flat window.
  return processor_->backend()->CreateGuestFunction(this, address);
}

}  // namespace xe::cpu

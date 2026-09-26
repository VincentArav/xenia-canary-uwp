/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_MODULE_H_
#define XENIA_CPU_MODULE_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/translation_domain.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Processor;

class Module {
 public:
  struct GuestFunctionInvalidationPlan {
   private:
    friend class Module;
    struct Replacement {
      size_t symbol_index;
      GuestFunction* old_function;
      std::unique_ptr<Function> new_function;
    };

    Module* module = nullptr;
    std::vector<Replacement> replacements;

   public:
    bool empty() const { return replacements.empty(); }
    // Guest addresses of the functions the plan replaces.
    std::vector<uint32_t> addresses() const;
  };

  // Selects the defined or failed guest functions an invalidation replaces.
  using GuestFunctionInvalidationFilter =
      std::function<bool(const GuestFunction&)>;

  explicit Module(Processor* processor,
                  TranslationDomainKey translation_domain_key =
                      TranslationDomainKey::DefaultDirect());
  virtual ~Module();

  Memory* memory() const { return memory_; }
  Processor* processor() const { return processor_; }
  const TranslationDomainKey& translation_domain_key() const {
    return translation_domain_key_;
  }

  virtual const std::string& name() const = 0;
  virtual bool is_executable() const = 0;

  virtual bool ContainsAddress(uint32_t address);

  Symbol* LookupSymbol(uint32_t address, bool wait = true);
  virtual Symbol::Status DeclareFunction(uint32_t address,
                                         Function** out_function);
  virtual Symbol::Status DeclareVariable(uint32_t address, Symbol** out_symbol);

  Symbol::Status DefineFunction(Function* symbol);
  Symbol::Status DefineVariable(Symbol* symbol);

  const std::vector<uint32_t> GetAddressedFunctions();
  std::vector<uint32_t> GetInstalledGuestFunctionAddresses(
      bool include_externs = true);
  bool PrepareGuestFunctionsForInvalidation(
      GuestFunctionInvalidationPlan* plan_out,
      const GuestFunctionInvalidationFilter& filter = {});
  size_t CommitGuestFunctionsForInvalidation(
      GuestFunctionInvalidationPlan&& plan);
  void RetireGuestFunctionsForUnload();
  void ForEachFunction(std::function<void(Function*)> callback);
  void ForEachSymbol(size_t start_index, size_t end_index,
                     std::function<void(Symbol*)> callback);
  size_t QuerySymbolCount();

  bool ReadMap(const char* file_name);

  virtual void Precompile() {}

 protected:
  virtual std::unique_ptr<Function> CreateFunction(uint32_t address) = 0;
  // Whether a replacement keeps the declaration metadata (name, end address,
  // save/restore kind). Modules whose code is rewritten at run time drop it,
  // so the new translation isn't truncated at the old end address.
  virtual bool preserves_declaration_metadata_on_invalidation() const {
    return true;
  }

  Processor* processor_ = nullptr;
  Memory* memory_ = nullptr;

 private:
  Symbol::Status DeclareSymbol(Symbol::Type type, uint32_t address,
                               Symbol** out_symbol);
  Symbol::Status DefineSymbol(Symbol* symbol);

  xe::global_critical_region global_critical_region_;
  // A Module has one immutable translation domain, so guest address alone is
  // sufficient for its private symbol namespace.
  std::unordered_map<uint32_t, Symbol*> map_;
  std::vector<std::unique_ptr<Symbol>> list_;
  // Replaced functions, kept until teardown: their native code and unwind
  // data stay registered.
  std::vector<std::unique_ptr<Symbol>> retired_list_;
  const TranslationDomainKey translation_domain_key_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_MODULE_H_

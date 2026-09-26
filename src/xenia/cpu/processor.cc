/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/guest_memory_access.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

// TODO(benvanik): based on compiler support
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

#if 0 && DEBUG
#define DEFAULT_DEBUG_FLAG true
#else
#define DEFAULT_DEBUG_FLAG false
#endif

DEFINE_bool(debug, DEFAULT_DEBUG_FLAG,
            "Allow debugging and retain debug information.", "General");
DEFINE_path(trace_function_data_path, "", "File to write trace data to.",
            "CPU");
DEFINE_bool(break_on_start, false, "Break into the debugger on startup.",
            "CPU");

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel

namespace cpu {

using xe::cpu::ppc::PPCOpcode;
using xe::kernel::XThread;

using namespace xe::literals;

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory),
      export_resolver_(export_resolver),
      translation_domain_registry_(this) {}

bool Processor::InstallCheckedGuestMemoryDispatcher(
    CheckedGuestMemoryDispatcher* dispatcher) {
  if (!dispatcher) {
    return false;
  }
  CheckedGuestMemoryDispatcher* expected = nullptr;
  const bool installed =
      checked_guest_memory_dispatcher_.compare_exchange_strong(
          expected, dispatcher, std::memory_order_acq_rel,
          std::memory_order_acquire);
  AdvanceCheckedExecutionEpoch();
  return installed;
}

bool Processor::UninstallCheckedGuestMemoryDispatcher(
    CheckedGuestMemoryDispatcher* dispatcher) {
  if (!dispatcher) {
    return false;
  }
  CheckedGuestMemoryDispatcher* expected = dispatcher;
  const bool uninstalled =
      checked_guest_memory_dispatcher_.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel,
          std::memory_order_acquire);
  AdvanceCheckedExecutionEpoch();
  return uninstalled;
}

bool Processor::CanExecuteTranslationDomain(
    TranslationDomainKey translation_domain_key) const {
  if (translation_domain_key == TranslationDomainKey::DefaultDirect()) {
    return true;
  }
  if (translation_domain_key.id() == TranslationDomainKey::kDefaultDirectId ||
      translation_domain_key.memory_access_mode() !=
          MemoryAccessMode::kChecked ||
      !backend_ || !backend_->SupportsCheckedGuestMemoryAccess()) {
    return false;
  }

  const TranslationDomainDescriptor* descriptor =
      translation_domain_registry_.Lookup(translation_domain_key);
  if (!descriptor ||
      descriptor->translation_domain_key() != translation_domain_key) {
    return false;
  }
  const GuestMemoryAccessor* accessor = descriptor->guest_memory_accessor();
  if (!accessor ||
      accessor->memory_access_mode() != MemoryAccessMode::kChecked) {
    return false;
  }

  CheckedGuestMemoryDispatcher* dispatcher = checked_guest_memory_dispatcher();
  return dispatcher && dispatcher->IsDomainExecutionAuthorized(
                           translation_domain_key, *descriptor);
}

bool Processor::CommitThreadTranslationDomain(
    ThreadState* thread_state, TranslationDomainKey target_domain_key,
    const TranslationDomainTransitionPermit* permit) {
  if (!thread_state || thread_state->processor() != this) {
    return false;
  }

  const TranslationDomainKey current_domain_key =
      thread_state->translation_domain_key();
  if (current_domain_key == target_domain_key && !permit) {
    if (!CanExecuteTranslationDomain(target_domain_key)) {
      return false;
    }
  } else {
    if (!permit || permit->expected_source_domain_key() != current_domain_key ||
        permit->target_domain_key() != target_domain_key) {
      return false;
    }

    const TranslationDomainDescriptor* target_descriptor = nullptr;
    if (target_domain_key != TranslationDomainKey::DefaultDirect()) {
      if (target_domain_key.id() == TranslationDomainKey::kDefaultDirectId ||
          target_domain_key.memory_access_mode() !=
              MemoryAccessMode::kChecked ||
          !backend_ || !backend_->SupportsCheckedGuestMemoryAccess()) {
        return false;
      }
      target_descriptor =
          translation_domain_registry_.Lookup(target_domain_key);
      const GuestMemoryAccessor* accessor =
          target_descriptor ? target_descriptor->guest_memory_accessor()
                            : nullptr;
      if (!target_descriptor || !accessor ||
          accessor->memory_access_mode() != MemoryAccessMode::kChecked) {
        return false;
      }
    }

    CheckedGuestMemoryDispatcher* dispatcher =
        checked_guest_memory_dispatcher();
    if (!dispatcher || !dispatcher->IsDomainTransitionAuthorized(
                           current_domain_key, target_domain_key,
                           target_descriptor, *permit)) {
      return false;
    }
  }

  auto global_lock = global_critical_region_.Acquire();
  if (thread_state->translation_domain_key() != current_domain_key) {
    return false;
  }
  const auto thread_info_it =
      thread_debug_infos_.find(thread_state->thread_id());
  if (thread_info_it == thread_debug_infos_.end() ||
      thread_info_it->second->translation_domain_key != current_domain_key) {
    return false;
  }

  thread_state->set_translation_domain_key(target_domain_key);
  thread_info_it->second->translation_domain_key = target_domain_key;
  return true;
}

Processor::~Processor() {
  // Tear the backend down without the Processor lock, which its workers may
  // need. Modules outlive it, keeping CodeCache metadata valid.
  stack_walker_.reset();
  frontend_.reset();
  backend_.reset();
  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
    retired_modules_.clear();
    builtin_module_ = nullptr;
  }

  if (functions_trace_file_) {
    functions_trace_file_->Flush();
    functions_trace_file_.reset();
  }
}

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  if (!stack_walker_) {
    // TODO(benvanik): disable features.
    if (cvars::debug) {
      XELOGW("Disabling --debug due to lack of stack walker");
      cvars::debug = false;
    }
  }

  // Open the trace data path, if requested.
  functions_trace_path_ = cvars::trace_function_data_path;
  if (!functions_trace_path_.empty()) {
    functions_trace_file_ =
        ChunkedMappedMemoryWriter::Open(functions_trace_path_, 32_MiB, true);
  }

  return true;
}

void Processor::PreLaunch() {
  if (cvars::break_on_start) {
    // Start paused.
    XELOGI("Breaking into debugger because of --break_on_start...");
    execution_state_ = ExecutionState::kRunning;
    Pause();
  } else {
    // Start running.
    execution_state_ = ExecutionState::kRunning;
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

void Processor::RemoveModule(const std::string_view name) {
  RemoveModule(TranslationDomainKey::DefaultDirect(), name);
}

void Processor::RemoveModule(TranslationDomainKey translation_domain_key,
                             const std::string_view name) {
  // For a checked domain, wait up to 2 s for in-flight resolutions without
  // blocking: the caller may hold the global lock a resolver needs.
  // DefaultDirect compiles are refused below.
  std::unique_lock resolution_lock(guest_code_resolution_mutex_,
                                   std::defer_lock);
  if (translation_domain_key != TranslationDomainKey::DefaultDirect()) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!resolution_lock.try_lock()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        XELOGE("Removing module '{}' without waiting for resolution", name);
        break;
      }
      std::this_thread::yield();
    }
  }
  auto global_lock = global_critical_region_.Acquire();

  auto itr = std::find_if(
      modules_.begin(), modules_.end(),
      [translation_domain_key,
       name](std::unique_ptr<xe::cpu::Module> const& module) {
        return module->translation_domain_key() == translation_domain_key &&
               module->name() == name;
      });

  if (itr == modules_.end()) {
    return;
  }

  Module* module = itr->get();
  std::vector<uint32_t> addressed_functions = module->GetAddressedFunctions();
  std::vector<uint32_t> installed_functions =
      module->GetInstalledGuestFunctionAddresses();
  std::sort(addressed_functions.begin(), addressed_functions.end());
  addressed_functions.erase(
      std::unique(addressed_functions.begin(), addressed_functions.end()),
      addressed_functions.end());
  std::sort(installed_functions.begin(), installed_functions.end());
  installed_functions.erase(
      std::unique(installed_functions.begin(), installed_functions.end()),
      installed_functions.end());

  if (entry_table_.HasInProgressCompilation(translation_domain_key)) {
    XELOGE("Refusing to remove module '{}' while guest code is compiling",
           name);
    return;
  }

  backend::CodeCache* code_cache = backend_ ? backend_->code_cache() : nullptr;
  // Resolver-only domains have no indirection slots; invalidating their
  // entries below is enough.
  if (backend::GetGuestCallDispatchMode(translation_domain_key) ==
          backend::GuestCallDispatchMode::kDirectLinkAndGlobalIndirection &&
      !installed_functions.empty() &&
      (!code_cache || !code_cache->ResetGuestIndirections(
                          translation_domain_key, installed_functions))) {
    XELOGE("Unable to reset guest indirections while removing module '{}'",
           name);
    return;
  }

  size_t invalidated_entry_count = 0;
  if (!entry_table_.InvalidateAddresses(translation_domain_key,
                                        addressed_functions,
                                        &invalidated_entry_count)) {
    XELOGE("Unable to invalidate guest entries while removing module '{}'",
           name);
    return;
  }

  module->RetireGuestFunctionsForUnload();
  std::unique_ptr<Module> retired_module = std::move(*itr);
  modules_.erase(itr);
  retired_modules_.push_back(std::move(retired_module));
}

Module* Processor::GetModule(const std::string_view name) {
  return GetModule(TranslationDomainKey::DefaultDirect(), name);
}

Module* Processor::GetModule(TranslationDomainKey translation_domain_key,
                             const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->translation_domain_key() == translation_domain_key &&
        module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  return QueryFunction(TranslationDomainKey::DefaultDirect(), address);
}

Function* Processor::QueryFunction(TranslationDomainKey translation_domain_key,
                                   uint32_t address) {
  if (!CanExecuteTranslationDomain(translation_domain_key)) {
    return nullptr;
  }
  auto entry = entry_table_.Get(translation_domain_key, address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return FindFunctionsWithAddress(TranslationDomainKey::DefaultDirect(),
                                  address);
}

std::vector<Function*> Processor::FindFunctionsWithAddress(
    TranslationDomainKey translation_domain_key, uint32_t address) {
  if (!CanExecuteTranslationDomain(translation_domain_key)) {
    return {};
  }
  return entry_table_.FindWithAddress(translation_domain_key, address);
}

void Processor::RemoveFunctionByAddress(uint32_t address) {
  RemoveFunctionByAddress(TranslationDomainKey::DefaultDirect(), address);
}

void Processor::RemoveFunctionByAddress(
    TranslationDomainKey translation_domain_key, uint32_t address) {
  entry_table_.Delete(translation_domain_key, address);
}

Function* Processor::ResolveFunction(uint32_t address) {
  return ResolveFunction(TranslationDomainKey::DefaultDirect(), address);
}

Function* Processor::ResolveFunction(
    TranslationDomainKey translation_domain_key, uint32_t address) {
  return ResolveFunction(translation_domain_key, address, nullptr);
}

bool Processor::ProbeCheckedInstructionFetch(
    TranslationDomainKey translation_domain_key, uint32_t address,
    ppc::GuestInstructionFetchFault* fault_out) const {
  // The domain's own reader: the probe word is not a translation source.
  const ppc::GuestCodeReader* reader =
      frontend_->guest_code_reader(translation_domain_key);
  if (!reader) {
    return false;
  }
  uint32_t instruction = 0;
  uint32_t source_address = 0;
  return !reader->TryReadInstructionWithFault(address, &instruction,
                                              &source_address, fault_out) &&
         fault_out->is_guest_fault();
}

bool Processor::DispatchGuestInstructionStorageFault(
    ppc::PPCContext* context, TranslationDomainKey translation_domain_key,
    const ppc::GuestInstructionFetchFault& fault) {
  if (!context ||
      translation_domain_key == TranslationDomainKey::DefaultDirect() ||
      !fault.is_guest_fault()) {
    return false;
  }
  CheckedGuestMemoryDispatcher* dispatcher = checked_guest_memory_dispatcher();
  return dispatcher && dispatcher->DispatchGuestInstructionStorageFault(
                           context, translation_domain_key, fault);
}

Function* Processor::ResolveFunction(
    TranslationDomainKey translation_domain_key, uint32_t address,
    ppc::GuestInstructionFetchFault* fetch_fault_out) {
  // A branch ignores the low two bits of its target.
  if (translation_domain_key != TranslationDomainKey::DefaultDirect()) {
    address &= ~uint32_t(3);
  }
  if (fetch_fault_out) {
    *fetch_fault_out = {};
    fetch_fault_out->instruction_address = address;
  }
  // Builtins are host handlers; resolving one lets a thread reenter straight
  // into it.
  if (translation_domain_key == TranslationDomainKey::DefaultDirect() &&
      address >= kBuiltinAddressBase && address < next_builtin_address_) {
    Symbol* symbol = builtin_module_->LookupSymbol(address, false);
    if (symbol && symbol->type() == Symbol::Type::kFunction &&
        static_cast<Function*>(symbol)->behavior() ==
            Function::Behavior::kBuiltin) {
      return static_cast<Function*>(symbol);
    }
    return nullptr;
  }

  std::shared_lock resolution_lock(guest_code_resolution_mutex_);
  if (!CanExecuteTranslationDomain(translation_domain_key)) {
    XELOGE(
        "Refusing to resolve guest function {:08X} in unsupported "
        "translation domain {}",
        address, translation_domain_key.id());
    return nullptr;
  }

  // An unfetchable checked-domain address faults at every branch to it and is
  // never cached as failed: the guest may map it later.
  if (translation_domain_key != TranslationDomainKey::DefaultDirect()) {
    if (Entry* ready = entry_table_.Get(translation_domain_key, address)) {
      return ready->function;
    }
    ppc::GuestInstructionFetchFault fault;
    if (ProbeCheckedInstructionFetch(translation_domain_key, address, &fault)) {
      if (fetch_fault_out) {
        *fetch_fault_out = fault;
      }
      return nullptr;
    }
  }

  Entry* entry;
  Entry::Status status =
      entry_table_.GetOrCreate(translation_domain_key, address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.

    // Grab symbol declaration.
    auto function = LookupFunction(translation_domain_key, address);

    if (!function || !DemandFunction(function)) {
      entry->status = Entry::STATUS_FAILED;
      if (translation_domain_key != TranslationDomainKey::DefaultDirect()) {
        std::lock_guard<std::mutex> pages_lock(checked_code_pages_mutex_);
        ++checked_failed_entry_counts_[translation_domain_key];
      }
      return nullptr;
    }
    // only add it to the list of resolved functions if resolving succeeded
    auto module_for = function->module();

    auto xexmod = dynamic_cast<XexModule*>(module_for);
    if (xexmod) {
      auto addr_flags = xexmod->GetInstructionAddressFlags(address);
      if (addr_flags) {
        addr_flags->was_resolved = 1;
      }
    }

    entry->function = function;
    entry->end_address = function->end_address();
    status = entry->status = Entry::STATUS_READY;
    if (translation_domain_key != TranslationDomainKey::DefaultDirect()) {
      XELOGD("Translated domain {:X} guest code {:08X}-{:08X}",
             translation_domain_key.id(), function->address(),
             function->end_address());
      RecordCheckedCodePages(*static_cast<GuestFunction*>(function));
    }
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}

bool Processor::HasLiveThreadInTranslationDomains(
    std::span<const TranslationDomainKey> translation_domain_keys) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& [thread_id, thread_info] : thread_debug_infos_) {
    if (thread_info->state == ThreadDebugInfo::State::kExited ||
        thread_info->state == ThreadDebugInfo::State::kZombie) {
      continue;
    }
    if (std::find(translation_domain_keys.begin(),
                  translation_domain_keys.end(),
                  thread_info->translation_domain_key) !=
        translation_domain_keys.end()) {
      return true;
    }
  }
  return false;
}

GuestCodeInvalidationResult Processor::InvalidateGuestCodeDomain(
    TranslationDomainKey translation_domain_key) {
  GuestCodeInvalidationResult result;
  const bool is_default_domain =
      translation_domain_key == TranslationDomainKey::DefaultDirect();
  const TranslationDomainDescriptor* checked_descriptor =
      is_default_domain
          ? nullptr
          : translation_domain_registry_.Lookup(translation_domain_key);
  const bool is_checked_resolver_only =
      checked_descriptor &&
      translation_domain_key.memory_access_mode() ==
          MemoryAccessMode::kChecked &&
      checked_descriptor->guest_memory_accessor() &&
      checked_descriptor->guest_memory_accessor()->memory_access_mode() ==
          MemoryAccessMode::kChecked &&
      backend::GetGuestCallDispatchMode(translation_domain_key) ==
          backend::GuestCallDispatchMode::kResolverOnly;
  if (!is_default_domain && !is_checked_resolver_only) {
    result.status = GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
    return result;
  }

  // Same lock order as ResolveFunction: resolution lock, then the global
  // lock.
  std::unique_lock resolution_lock(guest_code_resolution_mutex_);
  auto global_lock = global_critical_region_.Acquire();
  if (!is_default_domain && translation_domain_registry_.Lookup(
                                translation_domain_key) != checked_descriptor) {
    result.status = GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
    return result;
  }

  uint64_t* checked_generation = nullptr;
  if (is_default_domain) {
    result.generation =
        default_guest_code_generation_.load(std::memory_order_acquire);
  } else {
    checked_generation =
        &checked_guest_code_generations_[translation_domain_key];
    result.generation = *checked_generation;
  }

  if (is_default_domain && execution_state_ != ExecutionState::kPaused) {
    result.status = GuestCodeInvalidationStatus::kExecutionNotQuiescent;
    return result;
  }

  // DefaultDirect may hold direct calls, so no guest thread may be alive. A
  // checked domain may have no thread in it; the domain lock keeps threads
  // from committing into it meanwhile.
  for (const auto& [thread_id, thread_info] : thread_debug_infos_) {
    const bool is_live =
        thread_info->state != ThreadDebugInfo::State::kExited &&
        thread_info->state != ThreadDebugInfo::State::kZombie;
    if (is_live && (is_default_domain || thread_info->translation_domain_key ==
                                             translation_domain_key)) {
      result.status = GuestCodeInvalidationStatus::kLiveGuestThreads;
      return result;
    }
  }

  if (entry_table_.HasInProgressCompilation(translation_domain_key)) {
    result.status = GuestCodeInvalidationStatus::kCompilationInProgress;
    return result;
  }

  std::vector<uint32_t> guest_addresses;
  std::vector<Module*> domain_modules;
  for (const auto& module : modules_) {
    if (module->translation_domain_key() != translation_domain_key) {
      continue;
    }
    domain_modules.push_back(module.get());
    auto module_addresses = module->GetInstalledGuestFunctionAddresses(false);
    guest_addresses.insert(guest_addresses.end(), module_addresses.begin(),
                           module_addresses.end());
  }
  std::sort(guest_addresses.begin(), guest_addresses.end());
  guest_addresses.erase(
      std::unique(guest_addresses.begin(), guest_addresses.end()),
      guest_addresses.end());

  // Create every replacement before changing anything; the commit can't fail.
  std::vector<Module::GuestFunctionInvalidationPlan> replacement_plans(
      domain_modules.size());
  for (size_t i = 0; i < domain_modules.size(); ++i) {
    if (!domain_modules[i]->PrepareGuestFunctionsForInvalidation(
            &replacement_plans[i])) {
      result.status = GuestCodeInvalidationStatus::kFunctionReplacementFailed;
      return result;
    }
  }

  backend::CodeCache* code_cache = backend_ ? backend_->code_cache() : nullptr;
  // Only DefaultDirect has indirection slots to reset.
  if (is_default_domain && !guest_addresses.empty() &&
      (!code_cache || !code_cache->ResetGuestIndirections(
                          translation_domain_key, guest_addresses))) {
    result.status = GuestCodeInvalidationStatus::kBackendResetFailed;
    return result;
  }

  for (size_t i = 0; i < domain_modules.size(); ++i) {
    result.retired_function_count +=
        domain_modules[i]->CommitGuestFunctionsForInvalidation(
            std::move(replacement_plans[i]));
  }
  result.invalidated_entry_count =
      entry_table_.InvalidateDomain(translation_domain_key);
  if (is_default_domain) {
    result.generation =
        default_guest_code_generation_.fetch_add(1, std::memory_order_acq_rel) +
        1;
  } else {
    result.generation = ++*checked_generation;
    AdvanceCheckedExecutionEpoch();
    ForgetCheckedCodePages(translation_domain_key);
  }
  result.status = GuestCodeInvalidationStatus::kSuccess;
  return result;
}

bool Processor::IsCheckedResolverOnlyDomainLocked(
    TranslationDomainKey translation_domain_key) {
  if (translation_domain_key == TranslationDomainKey::DefaultDirect()) {
    return false;
  }
  const TranslationDomainDescriptor* descriptor =
      translation_domain_registry_.Lookup(translation_domain_key);
  return descriptor &&
         translation_domain_key.memory_access_mode() ==
             MemoryAccessMode::kChecked &&
         descriptor->guest_memory_accessor() &&
         descriptor->guest_memory_accessor()->memory_access_mode() ==
             MemoryAccessMode::kChecked &&
         backend::GetGuestCallDispatchMode(translation_domain_key) ==
             backend::GuestCallDispatchMode::kResolverOnly;
}

GuestCodeInvalidationResult Processor::InvalidateCheckedGuestCodeSources(
    uint32_t source_begin, uint32_t source_end) {
  GuestCodeInvalidationResult result;
  result.status = GuestCodeInvalidationStatus::kSuccess;
  // Titles without checked domains never take the resolution lock here.
  if (source_begin >= source_end ||
      !translation_domain_registry_.active_domain_count()) {
    return result;
  }
  return InvalidateCheckedGuestCode(
      [](TranslationDomainKey) { return true; },
      [source_begin, source_end](const GuestFunction& function) {
        return function.status() == Symbol::Status::kFailed ||
               function.CodeSourcesMayOverlap(source_begin, source_end);
      },
      false);
}

GuestCodeInvalidationResult
Processor::InvalidateRemappedCheckedGuestCodeDomains(
    std::span<const TranslationDomainKey> translation_domain_keys) {
  for (TranslationDomainKey translation_domain_key : translation_domain_keys) {
    if (translation_domain_key == TranslationDomainKey::DefaultDirect()) {
      GuestCodeInvalidationResult result;
      result.status =
          GuestCodeInvalidationStatus::kUnsupportedTranslationDomain;
      return result;
    }
  }
  // Probe each page holding translated code once. XeFu flushes after every
  // page it maps for x86 code and data, which almost never moves a code page,
  // so translations are scanned only when one did or a failed entry may now
  // resolve.
  std::unordered_map<TranslationDomainKey, std::vector<uint32_t>> moved_pages;
  bool retry_failed_entries = false;
  {
    std::lock_guard<std::mutex> pages_lock(checked_code_pages_mutex_);
    for (TranslationDomainKey translation_domain_key :
         translation_domain_keys) {
      auto failed_it =
          checked_failed_entry_counts_.find(translation_domain_key);
      if (failed_it != checked_failed_entry_counts_.end() &&
          failed_it->second) {
        failed_it->second = 0;
        retry_failed_entries = true;
      }
      auto pages_it = checked_code_pages_.find(translation_domain_key);
      if (pages_it == checked_code_pages_.end()) {
        continue;
      }
      const ppc::GuestCodeReader* reader =
          frontend_->guest_code_reader(translation_domain_key);
      auto& pages = pages_it->second;
      for (auto page_it = pages.begin(); page_it != pages.end();) {
        uint32_t instruction = 0;
        uint32_t source_address = ppc::kUnknownInstructionSource;
        if (reader && page_it->second != ppc::kUnknownInstructionSource &&
            reader->TryReadInstructionWithSource(page_it->first, &instruction,
                                                 &source_address) &&
            source_address == page_it->second) {
          ++page_it;
          continue;
        }
        moved_pages[translation_domain_key].push_back(page_it->first);
        page_it = pages.erase(page_it);
      }
    }
  }
  if (moved_pages.empty() && !retry_failed_entries) {
    GuestCodeInvalidationResult result;
    result.status = GuestCodeInvalidationStatus::kSuccess;
    return result;
  }
  return InvalidateCheckedGuestCode(
      [translation_domain_keys](TranslationDomainKey translation_domain_key) {
        return std::find(translation_domain_keys.begin(),
                         translation_domain_keys.end(),
                         translation_domain_key) !=
               translation_domain_keys.end();
      },
      [&moved_pages](const GuestFunction& function) {
        auto moved_it = moved_pages.find(function.translation_domain_key());
        if (moved_it == moved_pages.end()) {
          return false;
        }
        const uint32_t begin = function.address();
        const uint32_t end =
            function.has_end_address() ? function.end_address() : begin;
        for (uint32_t page : moved_it->second) {
          if (begin <= page + 0xFFF && end >= page) {
            return true;
          }
        }
        return false;
      },
      false);
}

void Processor::RecordCheckedCodePages(const GuestFunction& function) {
  const TranslationDomainKey translation_domain_key =
      function.translation_domain_key();
  const ppc::GuestCodeReader* reader =
      frontend_->guest_code_reader(translation_domain_key);
  const bool sources_known =
      reader && function.code_sources_complete() && function.has_end_address();
  const uint32_t begin = function.address();
  const uint32_t end =
      function.has_end_address() ? function.end_address() : begin;
  std::lock_guard<std::mutex> pages_lock(checked_code_pages_mutex_);
  auto& pages = checked_code_pages_[translation_domain_key];
  for (uint64_t page = begin & ~uint32_t(0xFFF); page <= end; page += 0x1000) {
    // Where the page's first word fetches from, if this translation read
    // its bytes from there.
    uint32_t source_page = ppc::kUnknownInstructionSource;
    const uint32_t probe = std::max(static_cast<uint32_t>(page), begin);
    uint32_t instruction = 0;
    uint32_t source_address = ppc::kUnknownInstructionSource;
    if (sources_known &&
        reader->TryReadInstructionWithSource(probe, &instruction,
                                             &source_address) &&
        source_address != ppc::kUnknownInstructionSource &&
        function.CodeSourcesMayOverlap(source_address, source_address + 4)) {
      source_page = source_address - (probe - static_cast<uint32_t>(page));
    }
    auto [page_it, inserted] =
        pages.try_emplace(static_cast<uint32_t>(page), source_page);
    if (!inserted && page_it->second != source_page) {
      // Translations of the page disagree: the next flush rechecks them.
      page_it->second = ppc::kUnknownInstructionSource;
    }
  }
}

void Processor::ForgetCheckedCodePages(
    TranslationDomainKey translation_domain_key) {
  std::lock_guard<std::mutex> pages_lock(checked_code_pages_mutex_);
  checked_code_pages_.erase(translation_domain_key);
  checked_failed_entry_counts_.erase(translation_domain_key);
}

bool Processor::CheckedGuestCodeSourcesCurrent(
    const GuestFunction& function, CheckedPageSources* page_sources) const {
  if (!function.code_sources_complete() || !function.has_end_address()) {
    return false;
  }
  const ppc::GuestCodeReader* reader =
      frontend_->guest_code_reader(function.translation_domain_key());
  if (!reader) {
    return false;
  }
  // The translation read every word of [address, end_address]; one word per
  // page shows where that page is mapped now.
  const uint32_t begin = function.address();
  const uint32_t end = function.end_address();
  for (uint64_t page = begin & ~uint32_t(0xFFF); page <= end; page += 0x1000) {
    const uint32_t probe = std::max(static_cast<uint32_t>(page), begin);
    const uint32_t probe_offset = probe - static_cast<uint32_t>(page);
    auto [it, inserted] = page_sources->try_emplace(
        {function.translation_domain_key(), static_cast<uint32_t>(page)},
        ppc::kUnknownInstructionSource);
    if (inserted) {
      uint32_t instruction = 0;
      uint32_t source_address = ppc::kUnknownInstructionSource;
      if (reader->TryReadInstructionWithSource(probe, &instruction,
                                               &source_address) &&
          source_address != ppc::kUnknownInstructionSource) {
        it->second = source_address - probe_offset;
      }
    }
    if (it->second == ppc::kUnknownInstructionSource) {
      return false;
    }
    const uint32_t source_address = it->second + probe_offset;
    if (!function.CodeSourcesMayOverlap(source_address, source_address + 4)) {
      return false;
    }
  }
  return true;
}

GuestCodeInvalidationResult Processor::InvalidateCheckedGuestCode(
    const std::function<bool(TranslationDomainKey)>& include_domain,
    const Module::GuestFunctionInvalidationFilter& select,
    bool invalidate_whole_domains) {
  GuestCodeInvalidationResult result;
  // The exclusive resolution lock waits out in-flight compiles, so a
  // translation that read the old bytes is published before this runs and is
  // retired by it. Neither lock is waited on while the other is held (a global
  // lock holder may be resolving); give up after 2 s rather than deadlock.
  std::unique_lock resolution_lock(guest_code_resolution_mutex_,
                                   std::defer_lock);
  global_unique_lock_type global_lock(global_critical_region::mutex(),
                                      std::defer_lock);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (uint32_t attempt = 0;; ++attempt) {
    if (resolution_lock.try_lock()) {
      if (global_lock.try_lock()) {
        break;
      }
      resolution_lock.unlock();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      XELOGE("Checked guest code invalidation could not acquire its locks");
      result.status = GuestCodeInvalidationStatus::kCompilationInProgress;
      return result;
    }
    if (attempt < 64) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  struct DomainPlan {
    Module* module;
    TranslationDomainKey translation_domain_key;
    Module::GuestFunctionInvalidationPlan plan;
  };
  std::vector<DomainPlan> plans;
  for (const auto& module : modules_) {
    const TranslationDomainKey translation_domain_key =
        module->translation_domain_key();
    if (!include_domain(translation_domain_key) ||
        !IsCheckedResolverOnlyDomainLocked(translation_domain_key)) {
      continue;
    }
    if (entry_table_.HasInProgressCompilation(translation_domain_key)) {
      result.status = GuestCodeInvalidationStatus::kCompilationInProgress;
      return result;
    }
    DomainPlan& domain_plan = plans.emplace_back();
    domain_plan.module = module.get();
    domain_plan.translation_domain_key = translation_domain_key;
    if (!module->PrepareGuestFunctionsForInvalidation(&domain_plan.plan,
                                                      select)) {
      result.status = GuestCodeInvalidationStatus::kFunctionReplacementFailed;
      return result;
    }
  }

  // Every plan exists, so nothing below can fail.
  for (DomainPlan& domain_plan : plans) {
    size_t invalidated_entry_count = 0;
    if (invalidate_whole_domains) {
      invalidated_entry_count =
          entry_table_.InvalidateDomain(domain_plan.translation_domain_key);
      ForgetCheckedCodePages(domain_plan.translation_domain_key);
    } else {
      const std::vector<uint32_t> addresses = domain_plan.plan.addresses();
      // Refused only while an entry is compiling, which the exclusive
      // resolution lock rules out.
      const bool invalidated =
          entry_table_.InvalidateAddresses(domain_plan.translation_domain_key,
                                           addresses, &invalidated_entry_count);
      assert_true(invalidated);
      (void)invalidated;
      invalidated_entry_count += entry_table_.InvalidateFailedEntries(
          domain_plan.translation_domain_key);
    }
    const size_t retired_function_count =
        domain_plan.module->CommitGuestFunctionsForInvalidation(
            std::move(domain_plan.plan));
    result.retired_function_count += retired_function_count;
    result.invalidated_entry_count += invalidated_entry_count;
    if (retired_function_count || invalidated_entry_count) {
      result.generation =
          ++checked_guest_code_generations_[domain_plan.translation_domain_key];
      AdvanceCheckedExecutionEpoch();
    }
  }
  result.status = GuestCodeInvalidationStatus::kSuccess;
  return result;
}

Module* Processor::LookupModule(uint32_t address) {
  return LookupModule(TranslationDomainKey::DefaultDirect(), address);
}

Module* Processor::LookupModule(TranslationDomainKey translation_domain_key,
                                uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
  // TODO(benvanik): cache last module low/high, as likely to be in there.
  for (const auto& module : modules_) {
    if (module->translation_domain_key() == translation_domain_key &&
        module->ContainsAddress(address)) {
      return module.get();
    }
  }
  return nullptr;
}
Function* Processor::LookupFunction(uint32_t address) {
  return LookupFunction(TranslationDomainKey::DefaultDirect(), address);
}

Function* Processor::LookupFunction(TranslationDomainKey translation_domain_key,
                                    uint32_t address) {
  if (!CanExecuteTranslationDomain(translation_domain_key)) {
    return nullptr;
  }

  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = LookupModule(translation_domain_key, address);

  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, translation_domain_key, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  return LookupFunction(module, module->translation_domain_key(), address);
}

Function* Processor::LookupFunction(Module* module,
                                    TranslationDomainKey translation_domain_key,
                                    uint32_t address) {
  if (!module || module->processor() != this ||
      module->translation_domain_key() != translation_domain_key ||
      !CanExecuteTranslationDomain(translation_domain_key)) {
    return nullptr;
  }

  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    auto* guest_function = static_cast<GuestFunction*>(function);
    // Externs are complete already; checked modules have none.
    if (guest_function->behavior() != Function::Behavior::kExtern) {
      if (!frontend_->DeclareFunction(guest_function)) {
        function->set_status(Symbol::Status::kFailed);
        return nullptr;
      }
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  if (!function || !function->module() ||
      function->module()->processor() != this ||
      !CanExecuteTranslationDomain(function->translation_domain_key())) {
    return false;
  }

  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    // Before we give the symbol back to the rest, let the debugger know.
    OnFunctionDefined(function);

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  if (!thread_state || thread_state->processor() != this) {
    return false;
  }
  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = ResolveAndCall(thread_state, address, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  if (!thread_state || thread_state->processor() != this) {
    return false;
  }
  return ResolveAndCall(thread_state, address, 0xBCBCBCBC, true);
}

bool Processor::ResolveAndCall(ThreadState* thread_state, uint32_t address,
                               uint32_t return_address,
                               bool deliver_fetch_faults) {
  const TranslationDomainKey translation_domain_key =
      thread_state->translation_domain_key();
  // A checked function retired between resolution and the call is rejected
  // before it runs; resolve its replacement and try again.
  constexpr int kMaxRetiredRetries = 16;
  for (int attempt = 0;; ++attempt) {
    ppc::GuestInstructionFetchFault fetch_fault;
    auto function =
        ResolveFunction(translation_domain_key, address,
                        deliver_fetch_faults ? &fetch_fault : nullptr);
    if (!function) {
      if (deliver_fetch_faults && fetch_fault.is_guest_fault()) {
        DispatchGuestInstructionStorageFault(
            thread_state->context(), translation_domain_key, fetch_fault);
      }
      // Symbol not found in any module.
      XELOGCPU("Execute({:08X}): failed to find function", address);
      return false;
    }
    const bool result = function->Call(thread_state, return_address);
    const bool retired = function->is_guest() &&
                         static_cast<GuestFunction*>(function)->is_retired();
    if (result || !retired || attempt == kMaxRetiredRetries) {
      return result;
    }
  }
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  if (!thread_state || thread_state->processor() != this ||
      (arg_count != 0 && !args)) {
    return 0xDEADBABE;
  }

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

bool Processor::Save(ByteStream* stream) {
  if (!CanSaveActiveTranslationDomains()) {
    XELOGE(
        "Processor save rejected: an unsupported translation domain is active");
    return false;
  }
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::CanSaveActiveTranslationDomains() {
  // Save states can't capture checked domains.
  if (translation_domain_registry_.active_domain_count() != 0) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();
  for (const auto& [thread_id, thread_info] : thread_debug_infos_) {
    if (thread_info->state == ThreadDebugInfo::State::kExited ||
        thread_info->state == ThreadDebugInfo::State::kZombie) {
      continue;
    }
    if (!CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
      return false;
    }
  }
  return true;
}

bool Processor::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    XELOGE("Processor::Restore - Invalid magic value!");
    return false;
  }

  // Clear cached thread data for zombie threads.
  std::vector<uint32_t> to_delete;
  for (auto& it : thread_debug_infos_) {
    if (it.second->state == ThreadDebugInfo::State::kZombie) {
      it.second->thread_handle = 0;
      to_delete.push_back(it.first);
    }
  }
  for (uint32_t thread_id : to_delete) {
    thread_debug_infos_.erase(thread_id);
  }

  return true;
}

uint8_t* Processor::AllocateFunctionTraceData(size_t size) {
  if (!functions_trace_file_) {
    return nullptr;
  }
  return functions_trace_file_->Allocate(size);
}

void Processor::OnFunctionDefined(Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
      if (function->ContainsAddress(breakpoint->guest_address())) {
        if (breakpoint->is_installed()) {
          backend_->InstallBreakpoint(breakpoint, function);
        }
      }
    }
  }
}

void Processor::OnThreadCreated(uint32_t thread_handle,
                                ThreadState* thread_state, Thread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto thread_info = std::make_unique<ThreadDebugInfo>();
  thread_info->thread_id = thread_state->thread_id();
  thread_info->translation_domain_key = thread_state->translation_domain_key();
  thread_info->thread = thread;
  thread_info->state = ThreadDebugInfo::State::kAlive;
  thread_info->suspended = false;
  thread_info->thread_handle = thread_handle;
  thread_debug_infos_.emplace(thread_info->thread_id, std::move(thread_info));
}

void Processor::OnThreadExit(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kExited;
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  it->second->thread_handle = 0;
  thread_debug_infos_.erase(it);
}

void Processor::OnThreadEnteringWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kWaiting;
}

void Processor::OnThreadLeavingWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  if (thread_info->state == ThreadDebugInfo::State::kWaiting) {
    thread_info->state = ThreadDebugInfo::State::kAlive;
  }
}

std::vector<ThreadDebugInfo*> Processor::QueryThreadDebugInfos() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<ThreadDebugInfo*> result;
  for (auto& it : thread_debug_infos_) {
    result.push_back(it.second.get());
  }
  return result;
}

ThreadDebugInfo* Processor::QueryThreadDebugInfo(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  const auto& it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void Processor::AddBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Add to breakpoints map.
  breakpoints_.push_back(breakpoint);

  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Resume();
  }
}

void Processor::RemoveBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Uninstall (if needed).
  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Suspend();
  }

  // Remove from breakpoint map.
  auto it = std::find(breakpoints_.begin(), breakpoints_.end(), breakpoint);
  breakpoints_.erase(it);
}

Breakpoint* Processor::FindBreakpoint(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address() == address) {
      return breakpoint;
    }
  }
  return nullptr;
}

void Processor::set_debug_listener(DebugListener* debug_listener) {
  if (debug_listener == debug_listener_) {
    return;
  }
  if (debug_listener_) {
    // Detach old debug listener.
    debug_listener_->OnDetached();
    debug_listener_ = nullptr;
  }
  if (debug_listener) {
    debug_listener_ = debug_listener;
  } else {
    if (execution_state_ == ExecutionState::kPaused) {
      XELOGI("Debugger detaching while execution is paused; continuing...");
      Continue();
    }
  }
}

void Processor::DemandDebugListener() {
  if (debug_listener_) {
    // Already present.
    debug_listener_->OnFocus();
    return;
  }
  if (!debug_listener_handler_) {
    XELOGE("Debugger demanded a listener but no handler was registered.");
    xe::debugging::Break();
    return;
  }
  set_debug_listener(debug_listener_handler_(this));
}

bool Processor::OnThreadBreakpointHit(Exception* ex) {
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all threads (but ourselves).
  SuspendAllThreads();

  // Lookup thread info block.
  auto it = thread_debug_infos_.find(ThreadState::GetThreadID());
  if (it == thread_debug_infos_.end()) {
    // Not found - exception on a thread we don't know about?
    assert_always("UD2 on a thread we don't track");
    return false;
  }
  auto thread_info = it->second.get();

  // Run through and uninstall all breakpoint UD2s to get us back to a clean
  // state.
  if (execution_state_ != ExecutionState::kStepping) {
    SuspendAllBreakpoints();
  }

  // Update all thread states with their latest values, using the context we
  // got from the exception instead of a sampled value (as it would just show
  // the exception handler).
  UpdateThreadExecutionStates(thread_info->thread_id, ex->thread_context());

  // Walk the captured thread stack and look for breakpoints at any address in
  // the stack. We just look for the first one.
  Breakpoint* breakpoint = nullptr;
  for (size_t i = 0; i < thread_info->frames.size(); ++i) {
    auto& frame = thread_info->frames[i];
    for (auto scan_breakpoint : breakpoints_) {
      if ((scan_breakpoint->address_type() == Breakpoint::AddressType::kGuest &&
           scan_breakpoint->guest_address() == frame.guest_pc) ||
          (scan_breakpoint->address_type() == Breakpoint::AddressType::kHost &&
           scan_breakpoint->host_address() == frame.host_pc)) {
        breakpoint = scan_breakpoint;
        break;
      }
    }
    if (breakpoint) {
      breakpoint->OnHit(thread_info, frame.host_pc);
      break;
    }
  }

  // We are waiting on the debugger now. Either wait for it to continue, add a
  // new step, or direct us somewhere else.
  // The debugger will ResumeAllThreads or just resume us (depending on what
  // it wants to do).
  execution_state_ = ExecutionState::kPaused;
  thread_info->suspended = true;

  // Must unlock, or we will deadlock.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  ResumeAllThreads();
  thread_info->thread->thread()->Suspend();

  // Apply thread context changes.
  // TODO(benvanik): apply to all threads?
#if XE_ARCH_AMD64
  ex->set_resume_pc(thread_info->host_context.rip + 2);
#elif XE_ARCH_ARM64
  ex->set_resume_pc(thread_info->host_context.pc + 2);
#else
#error Instruction pointer not specified for the target CPU architecture.
#endif  // XE_ARCH

  // Resume execution.
  return true;
}

void Processor::OnStepCompleted(ThreadDebugInfo* thread_info) {
  auto global_lock = global_critical_region_.Acquire();
  execution_state_ = ExecutionState::kPaused;
  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  // Note that we stay suspended.
}

bool Processor::OnUnhandledException(Exception* ex) {
  // If we have no listener return right away.
  // TODO(benvanik): DemandDebugListener()?
  if (!debug_listener_) {
    return false;
  }

  // If this isn't a managed thread, fail - let VS handle it for now.
  if (!Thread::IsInThread()) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Suspend all guest threads (but this one).
  SuspendAllThreads();

  UpdateThreadExecutionStates(Thread::GetCurrentThreadId(),
                              ex->thread_context());

  // Stop and notify the listener.
  // This will take control.
  assert_true(execution_state_ == ExecutionState::kRunning);
  execution_state_ = ExecutionState::kPaused;

  // Notify debugger that exceution stopped.
  // debug_listener_->OnException(info);
  debug_listener_->OnExecutionPaused();

  // Suspend self.
  Thread::GetCurrentThread()->thread()->Suspend();

  return true;
}

void Processor::ShowDebugger() {
  if (debug_listener_) {
    debug_listener_->OnFocus();
  } else {
    DemandDebugListener();
  }
}

bool Processor::SuspendAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (thread_info->suspended) {
      // Already suspended - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be suspended - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't suspend ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    bool did_suspend = thread->thread()->Suspend(nullptr);
    assert_true(did_suspend);
    thread_info->suspended = true;
  }
  return true;
}

bool Processor::ResumeThread(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return false;
  }
  auto thread_info = it->second.get();
  assert_true(thread_info->suspended);
  assert_false(thread_info->state == ThreadDebugInfo::State::kExited ||
               thread_info->state == ThreadDebugInfo::State::kZombie);
  thread_info->suspended = false;
  auto thread = thread_info->thread;
  return thread->thread()->Resume();
}

bool Processor::ResumeAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (!thread_info->suspended) {
      // Not suspended by us - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be resumed - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't resume ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    thread_info->suspended = false;
    bool did_resume = thread->thread()->Resume();
    assert_true(did_resume);
  }
  return true;
}

void Processor::UpdateThreadExecutionStates(
    uint32_t override_thread_id, HostThreadContext* override_context) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t frame_host_pcs[64];
  xe::cpu::StackFrame cpu_frames[64];
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    auto thread = thread_info->thread;
    if (!thread) {
      continue;
    }

    // Grab PPC context.
    // Note that this is only up to date if --store_all_context_values is
    // enabled (or --debug).
    if (thread->can_debugger_suspend()) {
      std::memcpy(&thread_info->guest_context,
                  thread->thread_state()->context(),
                  sizeof(thread_info->guest_context));
    }

    // Grab stack trace and X64 context then resolve all symbols.
    uint64_t hash;
    HostThreadContext* in_host_context = nullptr;
    if (override_thread_id == thread_info->thread_id) {
      // If we were passed an override context we use that. Otherwise, ask the
      // stack walker for a new context.
      in_host_context = override_context;
    }
    size_t count = stack_walker_->CaptureStackTrace(
        thread->thread()->native_handle(), frame_host_pcs, 0,
        xe::countof(frame_host_pcs), in_host_context,
        &thread_info->host_context, &hash);
    stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
    thread_info->frames.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& cpu_frame = cpu_frames[i];
      auto& frame = thread_info->frames[i];
      frame.host_pc = cpu_frame.host_pc;
      frame.host_function_address = cpu_frame.host_symbol.address;
      frame.guest_pc = cpu_frame.guest_pc;
      frame.guest_function_address = 0;
      frame.guest_function = nullptr;
      auto function = cpu_frame.guest_symbol.function;
      if (cpu_frame.type == cpu::StackFrame::Type::kGuest && function) {
        frame.guest_function_address = function->address();
        frame.guest_function = function;
      } else {
        std::strncpy(frame.name, cpu_frame.host_symbol.name,
                     xe::countof(frame.name));
        frame.name[xe::countof(frame.name) - 1] = 0;
      }
    }
  }
}

void Processor::SuspendAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Suspend();
  }
}

void Processor::ResumeAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Resume();
  }
}

void Processor::Pause() {
  {
    auto global_lock = global_critical_region_.Acquire();
    assert_true(execution_state_ == ExecutionState::kRunning);
    SuspendAllThreads();
    SuspendAllBreakpoints();
    UpdateThreadExecutionStates();
    execution_state_ = ExecutionState::kPaused;
    if (debug_listener_) {
      debug_listener_->OnExecutionPaused();
    }
  }
  DemandDebugListener();
}

void Processor::Continue() {
  auto global_lock = global_critical_region_.Acquire();
  if (execution_state_ == ExecutionState::kRunning) {
    return;
  } else if (execution_state_ == ExecutionState::kStepping) {
    assert_always("cancel stepping not done yet");
  }
  execution_state_ = ExecutionState::kRunning;
  ResumeAllBreakpoints();
  ResumeAllThreads();

  if (debug_listener_) {
    debug_listener_->OnExecutionContinued();
  }
}

void Processor::StepHostInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  if (!thread_info ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    execution_state_ = ExecutionState::kPaused;
    return;
  }
  uint64_t new_host_pc = backend_->CalculateNextHostInstruction(
      thread_info, thread_info->frames[0].host_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kHost, new_host_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

void Processor::StepGuestInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  if (!thread_info ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    execution_state_ = ExecutionState::kPaused;
    return;
  }

  uint32_t next_pc;
  if (!CalculateNextGuestInstruction(
          thread_info, thread_info->frames[0].guest_pc, &next_pc)) {
    execution_state_ = ExecutionState::kPaused;
    return;
  }

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kGuest, next_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

bool Processor::StepToGuestAddress(uint32_t thread_id, uint32_t pc) {
  auto thread_info = QueryThreadDebugInfo(thread_id);
  if (!thread_info ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    return false;
  }
  const TranslationDomainKey translation_domain_key =
      thread_info->translation_domain_key;
  auto functions = FindFunctionsWithAddress(translation_domain_key, pc);
  if (functions.empty()) {
    // Function hasn't been generated yet. Generate it.
    if (!ResolveFunction(translation_domain_key, pc)) {
      XELOGE(
          "Processor::StepToAddress({:08X}) - Function could not be resolved",
          pc);
      return false;
    }
  }

  // Instruct the thread to step forwards.
  threading::Fence fence;
  cpu::Breakpoint bp(
      this, Breakpoint::AddressType::kGuest, pc,
      [&fence](Breakpoint* breakpoint, ThreadDebugInfo* thread_info,
               uint64_t host_address) { fence.Signal(); });
  bp.Resume();

  // HACK
  uint32_t suspend_count = 1;
  while (suspend_count) {
    thread_info->thread->thread()->Resume(&suspend_count);
  }

  fence.Wait();
  bp.Suspend();

  return true;
}

bool Processor::StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc,
                                          uint32_t* new_pc_out) {
  if (!new_pc_out) {
    return false;
  }
  auto thread_info = QueryThreadDebugInfo(thread_id);
  if (!thread_info ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    return false;
  }
  const ppc::GuestCodeReader* guest_code_reader =
      frontend_->guest_code_reader(thread_info->translation_domain_key);
  if (!guest_code_reader) {
    return false;
  }
  xe::cpu::ppc::PPCDecodeData d;
  d.address = pc;
  if (!guest_code_reader->TryReadInstruction(d.address, &d.code)) {
    XELOGE("Failed to fetch PPC branch instruction at {:08X}", d.address);
    return false;
  }
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);

  // Must be on a branch.
  assert_true(xe::cpu::ppc::GetOpcodeInfo(opcode).group ==
              xe::cpu::ppc::PPCOpcodeGroup::kB);

  auto thread = thread_info->thread;
  auto context = thread->thread_state()->context();

  if (d.code == 0x4E800020) {
    // blr
    uint32_t nia = uint32_t(context->lr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (d.code == 0x4E800420) {
    // bctr
    uint32_t nia = uint32_t(context->ctr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bx) {
    // bx
    uint32_t nia = d.I.ADDR();
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bcx || opcode == PPCOpcode::bcctrx ||
             opcode == PPCOpcode::bclrx) {
    threading::Fence fence;
    auto callback = [&fence, &pc](Breakpoint* breakpoint,
                                  ThreadDebugInfo* thread_info,
                                  uint64_t host_address) {
      pc = breakpoint->guest_address();
      fence.Signal();
    };

    cpu::Breakpoint bpf(this, Breakpoint::AddressType::kGuest, pc + 4,
                        callback);
    bpf.Resume();

    uint32_t nia = 0;
    if (opcode == PPCOpcode::bcx) {
      // bcx
      nia = d.B.ADDR();
    } else if (opcode == PPCOpcode::bcctrx) {
      // bcctrx
      nia = uint32_t(context->ctr);
    } else if (opcode == PPCOpcode::bclrx) {
      // bclrx
      nia = uint32_t(context->lr);
    }

    cpu::Breakpoint bpt(this, Breakpoint::AddressType::kGuest, nia, callback);
    bpt.Resume();

    // HACK
    uint32_t suspend_count = 1;
    while (suspend_count) {
      thread->thread()->Resume(&suspend_count);
    }

    fence.Wait();
    bpt.Suspend();
    bpf.Suspend();
  }

  *new_pc_out = pc;
  return true;
}

uint32_t Processor::StepToGuestSafePoint(uint32_t thread_id, bool ignore_host) {
  // This cannot be done if we're the calling thread!
  if (thread_id == ThreadState::GetThreadID()) {
    assert_always(
        "Processor::StepToSafePoint(): target thread is the calling thread!");
    return 0;
  }
  auto thread_info = QueryThreadDebugInfo(thread_id);
  if (!thread_info ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    return 0;
  }
  const ppc::GuestCodeReader* guest_code_reader =
      frontend_->guest_code_reader(thread_info->translation_domain_key);
  if (!guest_code_reader) {
    return 0;
  }
  auto thread = thread_info->thread;

  // Now the fun part begins: Registers are only guaranteed to be synchronized
  // with the PPC context at a basic block boundary. Unfortunately, we most
  // likely stopped the thread at some point other than a boundary. We need to
  // step forward until we reach a boundary, and then perform the save.
  uint64_t frame_host_pcs[64];
  cpu::StackFrame cpu_frames[64];
  size_t count = stack_walker_->CaptureStackTrace(
      thread->thread()->native_handle(), frame_host_pcs, 0,
      xe::countof(frame_host_pcs), nullptr, nullptr);
  stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
  if (count == 0) {
    return 0;
  }

  auto& first_frame = cpu_frames[0];
  if (ignore_host) {
    for (size_t i = 0; i < count; i++) {
      if (cpu_frames[i].type == cpu::StackFrame::Type::kGuest &&
          cpu_frames[i].guest_pc) {
        first_frame = cpu_frames[i];
        break;
      }
    }
  }

  // Check if we're in guest code or host code.
  uint32_t pc = 0;
  if (first_frame.type == cpu::StackFrame::Type::kGuest) {
    auto& frame = first_frame;
    if (!frame.guest_pc) {
      // Lame. The guest->host thunk is a "guest" function.
      frame = cpu_frames[1];
    }

    pc = frame.guest_pc;

    // We're in guest code.
    // First: Find a synchronizing instruction and go to it.
    xe::cpu::ppc::PPCDecodeData d;
    const xe::cpu::ppc::PPCOpcodeInfo* sync_info = nullptr;
    d.address = cpu_frames[0].guest_pc - 4;
    do {
      d.address += 4;
      if (!guest_code_reader->TryReadInstruction(d.address, &d.code)) {
        XELOGE(
            "Failed to fetch PPC instruction at {:08X} while finding a safe "
            "point",
            d.address);
        return 0;
      }
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(d.code);
      if (opcode_info.type == cpu::ppc::PPCOpcodeType::kSync) {
        sync_info = &opcode_info;
        break;
      }
    } while (true);

    if (d.address != pc) {
      StepToGuestAddress(thread_id, d.address);
      pc = d.address;
    }

    // Okay. Now we're on a synchronizing instruction but we need to step
    // past it in order to get a synchronized context.
    // If we're on a branching instruction, it's guaranteed only going to have
    // two possible targets. For non-branching instructions, we can just step
    // over them.
    if (sync_info->group == xe::cpu::ppc::PPCOpcodeGroup::kB) {
      if (!StepIntoGuestBranchTarget(thread_id, d.address, &pc)) {
        return 0;
      }
    }
  } else {
    // We're in host code. Search backwards til we can get an idea of where
    // we are.
    cpu::GuestFunction* thunk_func = nullptr;
    cpu::Export* export_data = nullptr;
    uint32_t first_pc = 0;
    for (int i = 0; i < count; i++) {
      auto& frame = cpu_frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest && frame.guest_pc) {
        auto func = frame.guest_symbol.function;
        assert_true(func->is_guest());

        if (!first_pc) {
          first_pc = frame.guest_pc;
        }

        thunk_func = reinterpret_cast<cpu::GuestFunction*>(func);
        export_data = thunk_func->export_data();
        if (export_data) {
          break;
        }
      }
    }

    // If the export is blocking, we wrap up and save inside the export thunk.
    // When we're restored, we'll call the blocking export again.
    // Otherwise, we return from the thunk and save.
    if (export_data && export_data->tags & cpu::ExportTag::kBlocking) {
      pc = thunk_func->address();
    } else if (export_data) {
      // Non-blocking. Run until we return from the thunk.
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      StepToGuestAddress(thread_id, pc);
    } else if (first_pc) {
      // We're in the MMIO handler/mfmsr/something calling out of the guest
      // that doesn't use an export. If the current instruction is
      // synchronizing, we can just save here. Otherwise, step forward
      // (and call ourselves again so we run the correct logic).
      uint32_t code;
      if (!guest_code_reader->TryReadInstruction(first_pc, &code)) {
        XELOGE(
            "Failed to fetch PPC instruction at {:08X} while finding a safe "
            "point",
            first_pc);
        return 0;
      }
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(code);
      if (opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync) {
        // Good to go.
        pc = first_pc;
      } else {
        // Step forward and run this logic again.
        StepToGuestAddress(thread_id, first_pc + 4);
        return StepToGuestSafePoint(thread_id, true);
      }
    } else {
      // We've managed to catch a thread before it called into the guest.
      // Set a breakpoint on its startup procedure and capture it there.
      // TODO(DrChat): Reimplement
      assert_always("Unimplemented");
      /*
      auto creation_params = thread->creation_params();
      pc = creation_params->xapi_thread_startup
               ? creation_params->xapi_thread_startup
               : creation_params->start_address;
      StepToGuestAddress(thread_id, pc);
      */
    }
  }

  return pc;
}

bool TestPpcCondition(const xe::cpu::ppc::PPCContext* context, uint32_t bo,
                      uint32_t bi, bool check_ctr, bool check_cond) {
  bool ctr_ok = true;
  if (check_ctr) {
    if (select_bits(bo, 2, 2)) {
      ctr_ok = true;
    } else {
      uint32_t new_ctr_value = static_cast<uint32_t>(context->ctr - 1);
      if (select_bits(bo, 1, 1)) {
        ctr_ok = new_ctr_value == 0;
      } else {
        ctr_ok = new_ctr_value != 0;
      }
    }
  }
  bool cond_ok = true;
  if (check_cond) {
    if (select_bits(bo, 4, 4)) {
      cond_ok = true;
    } else {
      uint8_t cr = *(reinterpret_cast<const uint8_t*>(&context->cr0) +
                     (4 * (bi >> 2)) + (bi & 3));
      if (select_bits(bo, 3, 3)) {
        cond_ok = cr != 0;
      } else {
        cond_ok = cr == 0;
      }
    }
  }
  return ctr_ok && cond_ok;
}

bool Processor::CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                              uint32_t current_pc,
                                              uint32_t* next_pc_out) {
  if (!thread_info || !next_pc_out ||
      !CanExecuteTranslationDomain(thread_info->translation_domain_key)) {
    return false;
  }
  const ppc::GuestCodeReader* guest_code_reader =
      frontend_->guest_code_reader(thread_info->translation_domain_key);
  if (!guest_code_reader) {
    return false;
  }
  xe::cpu::ppc::PPCDecodeData d;
  d.address = current_pc;
  if (!guest_code_reader->TryReadInstruction(d.address, &d.code)) {
    XELOGE("Failed to fetch PPC instruction at {:08X} while stepping",
           d.address);
    return false;
  }
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);
  if (d.code == 0x4E800020) {
    // blr -- unconditional branch to LR.
    *next_pc_out = static_cast<uint32_t>(thread_info->guest_context.lr);
  } else if (d.code == 0x4E800420) {
    // bctr -- unconditional branch to CTR.
    *next_pc_out = static_cast<uint32_t>(thread_info->guest_context.ctr);
  } else if (opcode == PPCOpcode::bx) {
    // b/ba/bl/bla
    *next_pc_out = d.I.ADDR();
  } else if (opcode == PPCOpcode::bcx) {
    // bc/bca/bcl/bcla
    uint32_t target_pc = d.B.ADDR();
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.B.BO(),
                                        d.B.BI(), true, true);
    *next_pc_out = test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bclrx) {
    // bclr/bclrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), true, true);
    *next_pc_out = test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bcctrx) {
    // bcctr/bcctrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), false, true);
    *next_pc_out = test_passed ? target_pc : current_pc + 4;
  } else {
    *next_pc_out = current_pc + 4;
  }
  return true;
}
uint32_t Processor::GuestAtomicIncrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) + 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}
uint32_t Processor::GuestAtomicDecrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) - 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}

uint32_t Processor::GuestAtomicOr32(ppc::PPCContext* context,
                                    uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(
      xe::atomic_or(context->TranslateVirtual<volatile int32_t*>(guest_address),
                    xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicXor32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_xor(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicAnd32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_and(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}

bool Processor::GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                                 uint32_t new_value, uint32_t guest_address) {
  return xe::atomic_cas(xe::byte_swap(old_value), xe::byte_swap(new_value),
                        context->TranslateVirtual<uint32_t*>(guest_address));
}
}  // namespace cpu
}  // namespace xe

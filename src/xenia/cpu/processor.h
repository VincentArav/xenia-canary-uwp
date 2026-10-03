/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/entry_table.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/translation_domain_registry.h"
#include "xenia/memory.h"

DECLARE_bool(debug);

namespace xe {
namespace cpu {

constexpr fourcc_t kProcessorSaveSignature = make_fourcc("PROC");

class Breakpoint;
class StackWalker;
class XexModule;
struct GuestMemoryAccessRequest;
struct GuestMemoryAccessResult;

// Authorizes one domain switch, from expected_source_domain_key to
// target_domain_key. Its owner keeps the domains from retiring until the
// switch commits.
class TranslationDomainTransitionPermit {
 public:
  virtual ~TranslationDomainTransitionPermit() = default;
  virtual TranslationDomainKey expected_source_domain_key() const noexcept = 0;
  virtual TranslationDomainKey target_domain_key() const noexcept = 0;
};

// Receives the checked-memory faults of guest code. Leaves the guest call
// nonlocally and never returns (not noexcept: POSIX reentry unwinds).
class CheckedGuestMemoryFaultHandler {
 public:
  virtual ~CheckedGuestMemoryFaultHandler() = default;
  [[noreturn]] virtual void HandleGuestMemoryFault(
      ppc::PPCContext* context, TranslationDomainKey translation_domain_key,
      const GuestMemoryAccessRequest& request,
      const GuestMemoryAccessResult& result) = 0;
  // Raises an instruction storage fault for a branch to an address whose
  // instruction cannot be fetched, with the branch's LR and CTR updates in the
  // context. Leaves nonlocally when the fault is delivered; returns false when
  // nothing takes it.
  virtual bool DispatchGuestInstructionStorageFault(
      ppc::PPCContext* context, TranslationDomainKey translation_domain_key,
      const ppc::GuestInstructionFetchFault& fault) {
    return false;
  }
};

// Takes an interrupt on a thread whose ThreadState::guest_interrupt_pending
// was set. Called by checked-domain code at a branch, with the guest state
// complete in the context; resume_address is the branch target. May leave
// nonlocally (to run kernel code and later resume there) or return to take
// the branch normally.
class GuestInterruptPoller {
 public:
  virtual ~GuestInterruptPoller() = default;
  virtual void PollGuestInterrupt(ppc::PPCContext* context,
                                  uint32_t resume_address) = 0;
};

// Authorizes checked-domain execution and domain switches, and receives
// checked-memory faults; installed by the runtime that owns checked domains.
class CheckedGuestMemoryDispatcher : public CheckedGuestMemoryFaultHandler {
 public:
  ~CheckedGuestMemoryDispatcher() override = default;

  virtual bool IsDomainExecutionAuthorized(
      TranslationDomainKey translation_domain_key,
      const TranslationDomainDescriptor& descriptor) const noexcept = 0;
  virtual bool IsDomainTransitionAuthorized(
      TranslationDomainKey current_domain_key,
      TranslationDomainKey target_domain_key,
      const TranslationDomainDescriptor* target_descriptor,
      const TranslationDomainTransitionPermit& permit) const noexcept = 0;
};

enum class Irql : uint32_t {
  PASSIVE = 0,
  APC = 1,
  DISPATCH = 2,
  DPC = 3,
};

// Describes the current state of the emulator as known to the debugger.
// This determines which state the debugger is in and what operations are
// allowed.
enum class ExecutionState {
  // Target is running; the debugger is not waiting for any events.
  kRunning,
  // Target is running in stepping mode with the debugger waiting for feedback.
  kStepping,
  // Target is paused for debugging.
  kPaused,
  // Target has been stopped and cannot be restarted (crash, etc).
  kEnded,
};

enum class GuestCodeInvalidationStatus : uint8_t {
  kSuccess,
  kUnsupportedTranslationDomain,
  kExecutionNotQuiescent,
  kLiveGuestThreads,
  kCompilationInProgress,
  kBackendResetFailed,
  kFunctionReplacementFailed,
};

struct GuestCodeInvalidationResult {
  GuestCodeInvalidationStatus status =
      GuestCodeInvalidationStatus::kBackendResetFailed;
  uint64_t generation = 0;
  size_t retired_function_count = 0;
  size_t invalidated_entry_count = 0;

  explicit operator bool() const {
    return status == GuestCodeInvalidationStatus::kSuccess;
  }
};

// A page a TB flush names: the page holding a flushed effective address, at
// the size of the page that maps it.
struct FlushedGuestPageRange {
  uint32_t begin = 0;
  uint32_t size = 0;
};

class Processor {
 public:
  Processor(Memory* memory, ExportResolver* export_resolver);
  ~Processor();

  Memory* memory() const { return memory_; }
  StackWalker* stack_walker() const { return stack_walker_.get(); }
  ppc::PPCFrontend* frontend() const { return frontend_.get(); }
  backend::Backend* backend() const { return backend_.get(); }
  ExportResolver* export_resolver() const { return export_resolver_; }
  TranslationDomainRegistry* translation_domain_registry() {
    return &translation_domain_registry_;
  }
  const TranslationDomainRegistry* translation_domain_registry() const {
    return &translation_domain_registry_;
  }

  // Set and cleared while checked guest code is quiesced.
  void set_guest_interrupt_poller(GuestInterruptPoller* poller) {
    guest_interrupt_poller_.store(poller, std::memory_order_release);
  }
  GuestInterruptPoller* guest_interrupt_poller() const {
    return guest_interrupt_poller_.load(std::memory_order_acquire);
  }

  // Install fails while another dispatcher is installed; uninstall removes
  // only this one.
  bool InstallCheckedGuestMemoryDispatcher(
      CheckedGuestMemoryDispatcher* dispatcher);
  bool UninstallCheckedGuestMemoryDispatcher(
      CheckedGuestMemoryDispatcher* dispatcher);
  CheckedGuestMemoryDispatcher* checked_guest_memory_dispatcher() const {
    return checked_guest_memory_dispatcher_.load(std::memory_order_acquire);
  }

  // Changes whenever checked-domain code may stop running as resolved: a
  // translation retires, or a domain's execution authorization changes.
  // Resolutions cached under one value stay valid while it holds.
  const std::atomic<uint64_t>* checked_execution_epoch_address() const {
    return &checked_execution_epoch_;
  }
  uint64_t checked_execution_epoch() const {
    return checked_execution_epoch_.load(std::memory_order_acquire);
  }
  void AdvanceCheckedExecutionEpoch() {
    checked_execution_epoch_.fetch_add(1, std::memory_order_seq_cst);
    std::lock_guard<std::mutex> lock(checked_resolution_stale_flags_mutex_);
    for (uint32_t* flag : checked_resolution_stale_flags_) {
      std::atomic_ref<uint32_t>(*flag).store(1, std::memory_order_seq_cst);
    }
  }
  // Per-thread resolution tables' stale flags, set by every advance after it
  // is published (see Memory::RegisterTranslationStaleFlag).
  void RegisterCheckedResolutionStaleFlag(uint32_t* flag) {
    std::lock_guard<std::mutex> lock(checked_resolution_stale_flags_mutex_);
    checked_resolution_stale_flags_.push_back(flag);
  }
  void UnregisterCheckedResolutionStaleFlag(uint32_t* flag) {
    std::lock_guard<std::mutex> lock(checked_resolution_stale_flags_mutex_);
    std::erase(checked_resolution_stale_flags_, flag);
  }

  // DefaultDirect always; a checked domain while it is registered and the
  // installed dispatcher authorizes it.
  bool CanExecuteTranslationDomain(
      TranslationDomainKey translation_domain_key) const;

  // Switches the thread's domain once XThread has unwound the old frames,
  // updating ThreadState and its debugger mirror together. A switch needs a
  // matching permit; staying in the domain doesn't.
  bool CommitThreadTranslationDomain(
      ThreadState* thread_state, TranslationDomainKey target_domain_key,
      const TranslationDomainTransitionPermit* permit = nullptr);

  bool Setup(std::unique_ptr<backend::Backend> backend);

  // Runs any pre-launch logic once the module and thread have been setup.
  void PreLaunch();

  // The current execution state of the emulator.
  ExecutionState execution_state() const { return execution_state_; }

  // True if a debug listener is attached and the debugger is active.
  bool is_debugger_attached() const { return !!debug_listener_; }
  // Gets the active debug listener, if any.
  DebugListener* debug_listener() const { return debug_listener_; }
  // Sets the active debug listener, if any.
  // This can be used to detach the listener.
  void set_debug_listener(DebugListener* debug_listener);
  // Sets a handler that will be called from a random thread when a debugger
  // listener is required (such as on a breakpoint hit/etc).
  // Will only be called if the debug listener has not already been specified
  // with set_debug_listener.
  void set_debug_listener_request_handler(
      std::function<DebugListener*(Processor*)> handler) {
    debug_listener_handler_ = std::move(handler);
  }

  void set_debug_info_flags(uint32_t debug_info_flags) {
    debug_info_flags_ = debug_info_flags;
  }

  bool AddModule(std::unique_ptr<Module> module);
  void RemoveModule(const std::string_view name);
  void RemoveModule(TranslationDomainKey translation_domain_key,
                    const std::string_view name);
  Module* GetModule(const std::string_view name);
  Module* GetModule(TranslationDomainKey translation_domain_key,
                    const std::string_view name);
  std::vector<Module*> GetModules();

  Module* builtin_module() const { return builtin_module_; }
  Function* DefineBuiltin(const std::string_view name,
                          BuiltinFunction::Handler handler, void* arg0,
                          void* arg1);

  Function* QueryFunction(uint32_t address);
  Function* QueryFunction(TranslationDomainKey translation_domain_key,
                          uint32_t address);
  std::vector<Function*> FindFunctionsWithAddress(uint32_t address);
  std::vector<Function*> FindFunctionsWithAddress(
      TranslationDomainKey translation_domain_key, uint32_t address);
  void RemoveFunctionByAddress(uint32_t address);
  void RemoveFunctionByAddress(TranslationDomainKey translation_domain_key,
                               uint32_t address);

  Function* LookupFunction(uint32_t address);
  Function* LookupFunction(TranslationDomainKey translation_domain_key,
                           uint32_t address);
  Module* LookupModule(uint32_t address);
  Module* LookupModule(TranslationDomainKey translation_domain_key,
                       uint32_t address);
  Function* LookupFunction(Module* module, uint32_t address);
  Function* LookupFunction(Module* module,
                           TranslationDomainKey translation_domain_key,
                           uint32_t address);
  Function* ResolveFunction(uint32_t address);
  Function* ResolveFunction(TranslationDomainKey translation_domain_key,
                            uint32_t address);
  // Also reports why a checked-domain address cannot be resolved because its
  // instruction cannot be fetched. Such a failure is never cached: the next
  // resolution fetches again.
  Function* ResolveFunction(TranslationDomainKey translation_domain_key,
                            uint32_t address,
                            ppc::GuestInstructionFetchFault* fetch_fault_out);
  // Delivers an instruction storage fault in a checked domain through the
  // installed dispatcher. Returns only when nothing takes it.
  bool DispatchGuestInstructionStorageFault(
      ppc::PPCContext* context, TranslationDomainKey translation_domain_key,
      const ppc::GuestInstructionFetchFault& fault);

  // Retires every translation of a domain and advances its generation.
  // DefaultDirect code may hold direct calls between translations, so it is
  // flushed only with no guest thread alive; a checked domain, with no thread
  // in it. Native code and unwind records stay.
  GuestCodeInvalidationResult InvalidateGuestCodeDomain(
      TranslationDomainKey translation_domain_key);

  // Live invalidations of checked domains, while threads run in them: a
  // running thread reaches new code at its next branch out of a retired
  // function. The caller must hold neither the global nor the resolution lock.
  //
  // Retires the translations that read any guest physical byte in
  // [source_begin, source_end), plus failed ones.
  GuestCodeInvalidationResult InvalidateCheckedGuestCodeSources(
      uint32_t source_begin, uint32_t source_end);
  // A TB flush of checked domains: changes the address translation, not the
  // code bytes (icache sweeps retire those). Retires only the translations
  // whose instructions no longer fetch from the physical bytes they were
  // translated from, and the domains' failed entries. When the flush names
  // the pages it changed, only recorded code pages inside flushed_ranges are
  // probed; without them every recorded page is.
  GuestCodeInvalidationResult InvalidateRemappedCheckedGuestCodeDomains(
      std::span<const TranslationDomainKey> translation_domain_keys,
      std::optional<std::span<const FlushedGuestPageRange>> flushed_ranges =
          std::nullopt);
  bool HasLiveThreadInTranslationDomains(
      std::span<const TranslationDomainKey> translation_domain_keys);

  bool Execute(ThreadState* thread_state, uint32_t address);
  bool ExecuteRaw(ThreadState* thread_state, uint32_t address);
  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                   size_t arg_count);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  // Whether save states can represent every live thread's domain.
  bool CanSaveActiveTranslationDomains();

  // Returns a list of debugger info for all threads that have ever existed.
  // This is the preferred way to sample thread state vs. attempting to ask
  // the kernel.
  std::vector<ThreadDebugInfo*> QueryThreadDebugInfos();

  // Returns the debugger info for the given thread.
  ThreadDebugInfo* QueryThreadDebugInfo(uint32_t thread_id);

  // Adds a breakpoint to the debugger and activates it (if enabled).
  // The given breakpoint will not be owned by the debugger and must remain
  // allocated so long as it is added.
  void AddBreakpoint(Breakpoint* breakpoint);

  // Removes a breakpoint from the debugger and deactivates it.
  void RemoveBreakpoint(Breakpoint* breakpoint);

  // Finds a breakpoint that may be registered at the given address.
  Breakpoint* FindBreakpoint(uint32_t address);

  // Returns all currently registered breakpoints.
  std::vector<Breakpoint*> breakpoints() const;

  // Shows the debug listener, focusing it if it already exists.
  void ShowDebugger();

  // Pauses target execution by suspending all threads.
  // The debug listener will be requested if it has not been attached.
  void Pause();

  // Continues target execution from wherever it is.
  // This will cancel any active step operations and resume all threads.
  void Continue();

  // Steps the given thread a single x64 host instruction.
  // If the step is over a branch the branch will be followed.
  void StepHostInstruction(uint32_t thread_id);

  // Steps the given thread a single PPC guest instruction.
  // If the step is over a branch the branch will be followed.
  void StepGuestInstruction(uint32_t thread_id);

  // Steps the given thread until the guest address is hit.
  // Returns false if the step could not be completed (invalid target address).
  bool StepToGuestAddress(uint32_t thread_id, uint32_t pc);

  // Steps the given thread to the target of the branch at the specified guest
  // address. The address must specify a branch instruction.
  // Writes the new PC guest address and returns true on success. Returns false
  // without stepping if the branch instruction cannot be fetched.
  bool StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc,
                                 uint32_t* new_pc_out);

  // Steps the thread to a point where it's safe to terminate or read its
  // context. Returns the PC after we've finished stepping.
  // Pass true for ignore_host if you've stopped the thread yourself
  // in host code you want to ignore.
  // Returns the new PC guest address.
  uint32_t StepToGuestSafePoint(uint32_t thread_id, bool ignore_host = false);

  uint32_t GuestAtomicIncrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicDecrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicOr32(ppc::PPCContext* context, uint32_t guest_address,
                           uint32_t mask);
  uint32_t GuestAtomicXor32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  uint32_t GuestAtomicAnd32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  bool GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                        uint32_t new_value, uint32_t guest_address);

 public:
  // TODO(benvanik): hide.
  void OnThreadCreated(uint32_t handle, ThreadState* thread_state,
                       Thread* thread);
  void OnThreadExit(uint32_t thread_id);
  void OnThreadDestroyed(uint32_t thread_id);
  void OnThreadEnteringWait(uint32_t thread_id);
  void OnThreadLeavingWait(uint32_t thread_id);

  bool OnUnhandledException(Exception* ex);
  bool OnThreadBreakpointHit(Exception* ex);

  uint8_t* AllocateFunctionTraceData(size_t size);

 private:
  bool IsCheckedResolverOnlyDomainLocked(
      TranslationDomainKey translation_domain_key);
  // Called under a live invalidation's locks for each checked domain. Entry
  // addresses (sorted, unique) restrict select to those functions and keep
  // the domain's failed entries; nullopt offers select every function.
  using CheckedInvalidationCandidates =
      std::function<std::optional<std::vector<uint32_t>>(TranslationDomainKey)>;
  // Shared by the live checked invalidations. select chooses the defined
  // or failed functions to retire. A whole-domain invalidation tombstones
  // every entry; otherwise only the retired functions' entries and the
  // domain's failed entries, whose function may never have been declared.
  GuestCodeInvalidationResult InvalidateCheckedGuestCode(
      const std::function<bool(TranslationDomainKey)>& include_domain,
      const Module::GuestFunctionInvalidationFilter& select,
      bool invalidate_whole_domains,
      const CheckedInvalidationCandidates& candidates = {});
  // Records the pages and physical sources a new checked translation fetched
  // from, ending its compile.
  void RecordCheckedCodePages(const GuestFunction& function);
  void ForgetCheckedCodePages(TranslationDomainKey translation_domain_key);
  // The source index below; callers hold checked_code_pages_mutex_.
  void IndexCheckedCodeSourcesLocked(const GuestFunction& function);
  void UnindexCheckedCodeSourcesLocked(const GuestFunction& function);
  // Drops the domain's entries, or with addresses (sorted) only theirs.
  void UnindexCheckedCodeSourcesLocked(
      TranslationDomainKey translation_domain_key,
      const std::vector<uint32_t>* addresses);
  // The entry addresses (sorted, unique) of the domain's translations whose
  // sources overlap guest physical [begin, end); without a domain, those of
  // every domain.
  std::vector<uint32_t> CheckedCodeSourceOverlapsLocked(
      const TranslationDomainKey* translation_domain_key, uint32_t begin,
      uint32_t end) const;
  bool HasCheckedFailedEntriesLocked() const;

  // Resolves address in the thread's domain and calls it, re-resolving when a
  // concurrent checked-domain invalidation retired the function first.
  // With deliver_fetch_faults, an address whose instruction cannot be fetched
  // raises an instruction storage fault, as when reentering at it.
  bool ResolveAndCall(ThreadState* thread_state, uint32_t address,
                      uint32_t return_address,
                      bool deliver_fetch_faults = false);
  // Whether a checked-domain instruction fetch at address faults for a guest
  // reason, reported in fault_out.
  bool ProbeCheckedInstructionFetch(
      TranslationDomainKey translation_domain_key, uint32_t address,
      ppc::GuestInstructionFetchFault* fault_out) const;

  // Synchronously demands a debug listener.
  void DemandDebugListener();

  // Suspends all known threads (except the caller).
  bool SuspendAllThreads();
  // Resumes the given thread.
  bool ResumeThread(uint32_t thread_id);
  // Resumes all known threads (except the caller).
  bool ResumeAllThreads();
  // Updates all cached thread execution info (state, call stacks, etc).
  // The given override thread handle and context will be used in place of
  // sampled values for that thread.
  void UpdateThreadExecutionStates(
      uint32_t override_handle = 0,
      HostThreadContext* override_context = nullptr);

  // Suspends all breakpoints, uninstalling them as required.
  // No breakpoints will be triggered until they are resumed.
  void SuspendAllBreakpoints();
  // Resumes all breakpoints, re-installing them if required.
  void ResumeAllBreakpoints();

  void OnFunctionDefined(Function* function);

  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);
  void OnStepCompleted(ThreadDebugInfo* thread_info);
  void OnBreakpointHit(ThreadDebugInfo* thread_info, Breakpoint* breakpoint);

  // Calculates the next guest instruction based on the current thread state and
  // current PC. This will look for branches and other control flow
  // instructions.
  bool CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                     uint32_t current_pc,
                                     uint32_t* next_pc_out);

  bool DemandFunction(Function* function);

  Memory* memory_ = nullptr;
  std::unique_ptr<StackWalker> stack_walker_;

  std::function<DebugListener*(Processor*)> debug_listener_handler_;
  DebugListener* debug_listener_ = nullptr;

  // Which debug features are enabled in generated code.
  uint32_t debug_info_flags_ = 0;
  // If specified, the file trace data gets written to when running.
  std::filesystem::path functions_trace_path_;
  std::unique_ptr<ChunkedMappedMemoryWriter> functions_trace_file_;

  std::unique_ptr<ppc::PPCFrontend> frontend_;
  std::unique_ptr<backend::Backend> backend_;
  ExportResolver* export_resolver_ = nullptr;

  EntryTable entry_table_;
  // Shared by resolvers, held exclusively by invalidations.
  mutable std::shared_mutex guest_code_resolution_mutex_;
  std::atomic<uint64_t> default_guest_code_generation_{0};
  std::unordered_map<TranslationDomainKey, uint64_t>
      checked_guest_code_generations_;
  std::atomic<uint64_t> checked_execution_epoch_{1};
  std::mutex checked_resolution_stale_flags_mutex_;
  std::vector<uint32_t*> checked_resolution_stale_flags_;
  // Per checked domain, the physical 4 KiB page each effective page holding
  // translated code fetched from, so a TB flush probes pages rather than
  // every translation. kUnknownInstructionSource marks a page whose source is
  // unknown; a flush treats it as moved. Also counts each domain's failed
  // entries, which a flush retries.
  std::mutex checked_code_pages_mutex_;
  std::unordered_map<TranslationDomainKey,
                     std::unordered_map<uint32_t, uint32_t>>
      checked_code_pages_;
  std::unordered_map<TranslationDomainKey, size_t> checked_failed_entry_counts_;
  // Per guest physical 4 KiB page, the source bytes each live checked
  // translation read from it, so an icache sweep finds the translations it
  // affects without scanning them all. Translations whose sources are
  // incomplete overlap every sweep. Also counts the checked compiles that
  // have taken their entry but not yet recorded the outcome; a sweep that
  // sees one waits it out.
  struct CheckedCodeSource {
    TranslationAddress function;
    uint32_t begin;
    uint32_t end;
  };
  std::unordered_map<uint32_t, std::vector<CheckedCodeSource>>
      checked_code_sources_;
  std::vector<TranslationAddress> checked_unsourced_functions_;
  size_t checked_compiles_in_flight_ = 0;
  TranslationDomainRegistry translation_domain_registry_;
  std::atomic<GuestInterruptPoller*> guest_interrupt_poller_{nullptr};
  std::atomic<CheckedGuestMemoryDispatcher*> checked_guest_memory_dispatcher_{
      nullptr};
  xe::global_critical_region global_critical_region_;
  ExecutionState execution_state_ = ExecutionState::kPaused;
  std::vector<std::unique_ptr<Module>> modules_;
  // Removed modules retain Function objects referenced by append-only native
  // code metadata and by already-generated direct calls.
  std::vector<std::unique_ptr<Module>> retired_modules_;
  Module* builtin_module_ = nullptr;
  static constexpr uint32_t kBuiltinAddressBase = 0xFFFF0000u;
  uint32_t next_builtin_address_ = kBuiltinAddressBase;

  // Maps thread ID to state. Updated on thread create, and threads are never
  // removed. Must be guarded with the global lock.
  std::map<uint32_t, std::unique_ptr<ThreadDebugInfo>> thread_debug_infos_;
  // Guards each thread's domain key (in its ThreadState and ThreadDebugInfo)
  // and the thread map against domain commits, which take only this lock.
  // Adding or removing threads takes it with the global lock; reading another
  // thread's key takes it shared. The debugger's step paths read suspended
  // threads' keys without it.
  mutable std::shared_mutex thread_domain_mutex_;

  // TODO(benvanik): cleanup/change structures.
  std::vector<Breakpoint*> breakpoints_;

  Irql irql_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_

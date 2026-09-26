/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_STATE_H_
#define XENIA_CPU_THREAD_STATE_H_

#include <atomic>
#include <string>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/translation_domain.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Processor;

class ThreadState {
 public:
  ThreadState(Processor* processor, uint32_t thread_id, uint32_t stack_base = 0,
              uint32_t pcr_address = 0,
              TranslationDomainKey translation_domain_key =
                  TranslationDomainKey::DefaultDirect());
  ~ThreadState();

  Processor* processor() const { return processor_; }
  Memory* memory() const { return memory_; }
  void* backend_data() const { return backend_data_; }
  ppc::PPCContext* context() const { return context_; }
  uint32_t thread_id() const { return thread_id_; }
  TranslationDomainKey translation_domain_key() const {
    return translation_domain_key_;
  }

  // Set by another thread to interrupt this one: checked-domain code polls it
  // at its next branch and hands control to the processor's
  // GuestInterruptPoller.
  bool guest_interrupt_pending() const {
    return guest_interrupt_pending_.load(std::memory_order_acquire);
  }
  void set_guest_interrupt_pending(bool pending) {
    guest_interrupt_pending_.store(pending, std::memory_order_release);
    context_->guest_interrupt_pending = pending ? 1 : 0;
  }

  // How many Processor::ResolveAndCall frames are active on this thread: each
  // host-to-guest dispatch, including guest code run from host code (DPCs,
  // APCs, callbacks) nested inside another. Owner thread only.
  uint32_t guest_dispatch_depth() const { return guest_dispatch_depth_; }
  void set_guest_dispatch_depth(uint32_t depth) {
    guest_dispatch_depth_ = depth;
  }

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();

 private:
  friend class Processor;

  // Set by Processor::CommitThreadTranslationDomain.
  void set_translation_domain_key(TranslationDomainKey translation_domain_key) {
    translation_domain_key_ = translation_domain_key;
  }

  Processor* processor_;
  Memory* memory_;
  void* backend_data_;

  uint32_t pcr_address_ = 0;
  uint32_t thread_id_ = 0;
  TranslationDomainKey translation_domain_key_;
  std::atomic<bool> guest_interrupt_pending_{false};
  uint32_t guest_dispatch_depth_ = 0;

  // NOTE: must be 64b aligned for SSE ops.
  ppc::PPCContext* context_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_STATE_H_

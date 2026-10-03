/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);
  // Backs a guest KTIMER (dispatcher type 8 or 9) first used without a handle.
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume);
  X_STATUS Cancel();

  // KeSetTimerEx: arms the timer with a DPC queued at each expiry. Returns
  // whether the timer was already armed.
  bool SetGuestTimer(int64_t due_time, uint32_t period_ms, uint32_t dpc_ptr);
  // KeCancelTimer: returns whether the timer was armed.
  bool CancelGuestTimer();

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override;
  void WaitCallback() override;
  std::chrono::milliseconds WaitPacingDelay() override;

 private:
  void OnGuestTimerExpired(uint64_t generation, uint32_t dpc_ptr,
                           bool periodic);
  // Expiries of the paced timer by now; 0 before the first.
  uint64_t PacedExpiryCountLocked(
      std::chrono::steady_clock::time_point now) const;

  std::unique_ptr<xe::threading::Timer> timer_;
  std::mutex timer_lock_;
  bool notification_ = false;

  // A periodic notification timer stays signaled after its first expiry, so a
  // loop waiting on it spins. In XeFu sessions, a thread the timer already
  // satisfied since its latest expiry is held off until the next one, as if
  // preempted; a wait the timer wouldn't satisfy is unaffected.
  bool pacing_ = false;
  std::chrono::steady_clock::time_point paced_first_expiry_;
  std::chrono::steady_clock::duration paced_period_{};
  // Per waiting thread, the expiry count when the timer last satisfied it.
  std::vector<std::pair<uint32_t, uint64_t>> paced_waiters_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;

  // Guest KTIMER state. timer_ provides the waitable signal state; DPCs are
  // delivered from a timer-queue item because a Win32 waitable-timer
  // completion routine only runs in an alertable wait of the arming thread.
  // The generation invalidates callbacks from an earlier arming that may
  // already be running when the timer is reset.
  uint32_t guest_timer_ptr_ = 0;
  bool guest_timer_armed_ = false;
  uint64_t guest_timer_generation_ = 0;
  // Guest armings arm the host timer only once a thread has waited on the
  // timer; until then the arming is recorded here, and the first wait brings
  // the host timer up to date.
  bool host_timer_waited_ = false;
  bool lazy_pending_ = false;
  bool lazy_signaled_ = false;
  xe::chrono::WinSystemClock::time_point lazy_due_;
  uint32_t lazy_period_ms_ = 0;
  // Thread that armed the timer. Its DPC targets that thread's processor.
  uint32_t guest_timer_dpc_thread_id_ = 0;
  std::weak_ptr<xe::threading::TimerQueueWaitItem> guest_timer_wait_item_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_

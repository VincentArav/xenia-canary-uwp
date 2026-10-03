/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <algorithm>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_mode_kernel_runtime.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  if (auto wait_item = guest_timer_wait_item_.lock()) {
    wait_item->Disarm();
  }
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  notification_ = timer_type == 0;
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

namespace {

// Negative due times are relative 100ns intervals; positive ones are absolute
// guest system times. Any timer implementation uses absolute times eventually,
// so convert as early as possible for increased accuracy.
xe::chrono::WinSystemClock::time_point GuestDueTimeToHost(int64_t due_time) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;
  if (due_time < 0) {
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    return date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  }
  return date::clock_cast<WinSystemClock>(
      XSystemClock::from_file_time(due_time));
}

}  // namespace

void XTimer::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  // TimerNotificationObject = 8, TimerSynchronizationObject = 9.
  Initialize(header->type - 8);
  guest_timer_ptr_ = memory()->HostToGuestVirtual(native_ptr);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::WinSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  const WinSystemClock::time_point due_tp = GuestDueTimeToHost(due_time);

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    callback = [cb_thread, cb_routine, cb_routine_arg]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  host_timer_waited_ = true;
  lazy_pending_ = false;
  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  pacing_ = result && period_ms && notification_ &&
            kernel_state()->emulator()->mounted_xefu_session();
  paced_waiters_.clear();
  if (pacing_) {
    paced_first_expiry_ =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::max(due_tp - WinSystemClock::now(),
                     WinSystemClock::duration::zero()));
    paced_period_ = std::chrono::milliseconds(period_ms);
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  pacing_ = false;
  paced_waiters_.clear();
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

xe::threading::WaitHandle* XTimer::GetWaitHandle() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!host_timer_waited_) {
    host_timer_waited_ = true;
    if (lazy_pending_ || lazy_signaled_) {
      // A signaled timer fires at once; a periodic one keeps its period.
      const auto due =
          lazy_signaled_ ? xe::chrono::WinSystemClock::now() : lazy_due_;
      const bool armed =
          lazy_pending_ && lazy_period_ms_
              ? timer_->SetRepeatingAt(
                    due, std::chrono::milliseconds(lazy_period_ms_), nullptr)
              : timer_->SetOnceAt(due, nullptr);
      if (!armed) {
        XELOGE("XTimer: failed to arm the host timer for a wait");
      }
    }
  }
  return timer_.get();
}

uint64_t XTimer::PacedExpiryCountLocked(
    std::chrono::steady_clock::time_point now) const {
  if (now < paced_first_expiry_) {
    return 0;
  }
  return 1 + static_cast<uint64_t>((now - paced_first_expiry_) / paced_period_);
}

std::chrono::milliseconds XTimer::WaitPacingDelay() {
  XThread* thread = XThread::GetCurrentThread();
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!pacing_ || !thread) {
    return std::chrono::milliseconds(0);
  }
  const auto now = std::chrono::steady_clock::now();
  const uint64_t expiries = PacedExpiryCountLocked(now);
  for (const auto& [thread_id, seen] : paced_waiters_) {
    if (thread_id == thread->thread_id()) {
      if (!expiries || seen < expiries) {
        break;
      }
      const auto next =
          paced_first_expiry_ + paced_period_ * static_cast<int64_t>(seen);
      return std::chrono::ceil<std::chrono::milliseconds>(next - now);
    }
  }
  return std::chrono::milliseconds(0);
}

void XTimer::WaitCallback() {
  XThread* thread = XThread::GetCurrentThread();
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!pacing_ || !thread) {
    return;
  }
  // The timer satisfied the wait, so it has expired at least once.
  const uint64_t expiries = std::max<uint64_t>(
      PacedExpiryCountLocked(std::chrono::steady_clock::now()), 1);
  for (auto& [thread_id, seen] : paced_waiters_) {
    if (thread_id == thread->thread_id()) {
      seen = expiries;
      return;
    }
  }
  if (paced_waiters_.size() >= 64) {
    paced_waiters_.clear();
  }
  paced_waiters_.emplace_back(thread->thread_id(), expiries);
}

bool XTimer::SetGuestTimer(int64_t due_time, uint32_t period_ms,
                           uint32_t dpc_ptr) {
  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  const bool periodic = period_ms != 0;

  bool was_armed;
  uint64_t generation;
  std::shared_ptr<xe::threading::TimerQueueWaitItem> previous_item;
  {
    std::lock_guard<std::mutex> lock(timer_lock_);
    pacing_ = false;
    paced_waiters_.clear();
    was_armed = guest_timer_armed_;
    generation = ++guest_timer_generation_;
    guest_timer_armed_ = true;
    // A DPC that ran on a host thread (the timer DPC fallback) and re-arms
    // its timer keeps the timer on the guest thread it targets.
    XThread* current_thread = XThread::GetCurrentThread();
    if (current_thread && current_thread->is_guest_thread()) {
      guest_timer_dpc_thread_id_ = current_thread->thread_id();
    }
    previous_item = guest_timer_wait_item_.lock();
    guest_timer_wait_item_.reset();
    lazy_pending_ = false;
    lazy_signaled_ = false;
    if (guest_timer_ptr_) {
      // Mirror the fields guest code may read inline.
      auto guest_timer =
          memory()->TranslateVirtual<X_KTIMER*>(guest_timer_ptr_);
      guest_timer->header.signal_state = 0;
      guest_timer->header.inserted = 1;
      guest_timer->period = period_ms;
    }
  }
  // Disarm blocks while a callback runs, and callbacks take timer_lock_.
  if (previous_item) {
    previous_item->Disarm();
  }

  // Guest timers expire on clock interrupts: the first 1 ms tick boundary at or
  // after the due time, and never before the next one. The grid is fixed, so a
  // timer re-armed from its own DPC keeps its rate.
  using WinDuration = xe::chrono::WinSystemClock::duration;
  const int64_t tick =
      std::chrono::duration_cast<WinDuration>(std::chrono::milliseconds(1))
          .count();
  const auto now = xe::chrono::WinSystemClock::now();
  const int64_t now_count = now.time_since_epoch().count();
  const int64_t target =
      GuestDueTimeToHost(due_time).time_since_epoch().count();
  int64_t boundary = (target + tick - 1) / tick * tick;
  if (boundary <= now_count) {
    boundary = (now_count / tick + 1) * tick;
  }
  const WinDuration delay(boundary - now_count);
  const auto due_tp = now + delay;

  // The host waitable timer only provides the signal state for guest waits.
  {
    std::lock_guard<std::mutex> lock(timer_lock_);
    if (generation == guest_timer_generation_) {
      if (host_timer_waited_) {
        const bool armed =
            periodic
                ? timer_->SetRepeatingAt(
                      due_tp, std::chrono::milliseconds(period_ms), nullptr)
                : timer_->SetOnceAt(due_tp, nullptr);
        if (!armed) {
          XELOGE("KeSetTimerEx: failed to arm the host timer");
        }
      } else {
        lazy_pending_ = true;
        lazy_due_ = due_tp;
        lazy_period_ms_ = period_ms;
      }
    }
  }

  // Deliver the DPC from the timer queue. Look the timer up by handle on
  // expiry: the queue item may outlive a released guest object, and only the
  // latest arming may deliver.
  using QueueClock = xe::threading::TimerQueueWaitItem::clock;
  const auto queue_due =
      QueueClock::now() +
      std::chrono::duration_cast<QueueClock::duration>(delay);
  const uint32_t timer_handle = handle();
  KernelState* const state = kernel_state();
  auto callback = [state, timer_handle, generation, dpc_ptr, periodic](void*) {
    auto timer = state->object_table()->LookupObject<XTimer>(timer_handle);
    if (timer) {
      timer->OnGuestTimerExpired(generation, dpc_ptr, periodic);
    }
  };
  std::weak_ptr<xe::threading::TimerQueueWaitItem> wait_item =
      periodic ? xe::threading::QueueTimerRecurring(
                     std::move(callback), nullptr, queue_due,
                     std::chrono::milliseconds(period_ms))
               : xe::threading::QueueTimerOnce(std::move(callback), nullptr,
                                               queue_due);

  bool superseded;
  {
    std::lock_guard<std::mutex> lock(timer_lock_);
    superseded = generation != guest_timer_generation_;
    if (!superseded) {
      guest_timer_wait_item_ = wait_item;
    }
  }
  if (superseded) {
    // Another KeSetTimerEx or KeCancelTimer won the race.
    if (auto item = wait_item.lock()) {
      item->Disarm();
    }
  }
  return was_armed;
}

bool XTimer::CancelGuestTimer() {
  bool was_armed;
  std::shared_ptr<xe::threading::TimerQueueWaitItem> wait_item;
  {
    std::lock_guard<std::mutex> lock(timer_lock_);
    was_armed = guest_timer_armed_;
    guest_timer_armed_ = false;
    ++guest_timer_generation_;
    wait_item = guest_timer_wait_item_.lock();
    guest_timer_wait_item_.reset();
    if (guest_timer_ptr_) {
      memory()->TranslateVirtual<X_KTIMER*>(guest_timer_ptr_)->header.inserted =
          0;
    }
    // Cancelling keeps the signal state.
    lazy_pending_ = false;
    if (host_timer_waited_) {
      timer_->Cancel();
    }
  }
  if (wait_item) {
    wait_item->Disarm();
  }
  return was_armed;
}

void XTimer::OnGuestTimerExpired(uint64_t generation, uint32_t dpc_ptr,
                                 bool periodic) {
  uint32_t dpc_thread_id;
  {
    std::lock_guard<std::mutex> lock(timer_lock_);
    dpc_thread_id = guest_timer_dpc_thread_id_;
    if (generation != guest_timer_generation_ || !guest_timer_armed_) {
      return;
    }
    if (!periodic) {
      guest_timer_armed_ = false;
      lazy_pending_ = false;
    }
    lazy_signaled_ = true;
    if (guest_timer_ptr_) {
      auto guest_timer =
          memory()->TranslateVirtual<X_KTIMER*>(guest_timer_ptr_);
      guest_timer->header.signal_state = 1;
      guest_timer->header.inserted = periodic ? 1 : 0;
    }
  }
  if (dpc_ptr) {
    // Like NT, pass the expiry system time as the DPC's system arguments.
    const uint64_t time = Clock::QueryGuestSystemTime();
    const uint32_t time_low = static_cast<uint32_t>(time);
    const uint32_t time_high = static_cast<uint32_t>(time >> 32);
    // The DPC interrupts the arming thread if it runs user-mode code;
    // otherwise it runs on the timer DPC thread.
    UserModeKernelRuntime* runtime = kernel_state()->user_mode_kernel_runtime();
    if (!dpc_thread_id || !runtime ||
        !runtime->QueueThreadDpc(dpc_thread_id, dpc_ptr, time_low, time_high)) {
      kernel_state()->QueueTimerDpc(dpc_ptr, time_low, time_high);
    }
  }
}

}  // namespace kernel
}  // namespace xe

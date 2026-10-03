/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/user_mode_manager.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/memory.h"

namespace xe::kernel {
namespace {

struct PendingMappingRetirement {
  UserModeMappingHandle mapping;
  std::shared_ptr<UserModeMappingRetirementListener> listener;
};

using PendingMappingRetirements = std::array<PendingMappingRetirement, 2>;

void NotifyMappingRetirements(
    const PendingMappingRetirements& pending_retirements) noexcept {
  for (const PendingMappingRetirement& pending : pending_retirements) {
    if (pending.listener) {
      pending.listener->OnUserModeMappingRetired(pending.mapping);
    }
  }
}

}  // namespace

UserModeMappingRetirementListenerAttachmentDiagnostic
UserModeMappingGenerationToken::AttachRetirementListener(
    const std::weak_ptr<UserModeMappingRetirementListener>& listener) const {
  const std::shared_ptr<UserModeMappingRetirementListener> retained_listener =
      listener.lock();
  if (!retained_listener) {
    return UserModeMappingRetirementListenerAttachmentDiagnostic::
        kInvalidListener;
  }

  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!current_.load(std::memory_order_acquire)) {
    return UserModeMappingRetirementListenerAttachmentDiagnostic::
        kMappingNotCurrent;
  }
  if (retirement_listener_attached_) {
    return UserModeMappingRetirementListenerAttachmentDiagnostic::
        kListenerAlreadyAttached;
  }
  retirement_listener_ = retained_listener;
  retirement_listener_attached_ = true;
  return UserModeMappingRetirementListenerAttachmentDiagnostic::kNone;
}

std::shared_ptr<UserModeMappingRetirementListener>
UserModeMappingGenerationToken::RetireAndTakeRetirementListener() {
  std::unique_lock<std::mutex> lock(lifecycle_mutex_);
  if (!current_.exchange(false, std::memory_order_acq_rel)) {
    return nullptr;
  }
  // Generated code uses cached translations without checking the mapping.
  Memory::AdvanceTranslationEpoch();
  lifecycle_condition_.wait(lock,
                            [this] { return active_operation_count_ == 0; });
  return retirement_listener_.lock();
}

bool UserModeMappingGenerationToken::TryAcquireOperation() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!current_.load(std::memory_order_acquire)) {
    return false;
  }
  ++active_operation_count_;
  return true;
}

void UserModeMappingGenerationToken::ReleaseOperation() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  assert_true(active_operation_count_ != 0);
  --active_operation_count_;
  if (active_operation_count_ == 0) {
    lifecycle_condition_.notify_all();
  }
}

UserModeMappingGenerationOperationLease::
    UserModeMappingGenerationOperationLease(
        std::shared_ptr<const UserModeMappingGenerationToken> token) {
  if (token && token->TryAcquireOperation()) {
    token_ = std::move(token);
  }
}

UserModeMappingGenerationOperationLease::
    ~UserModeMappingGenerationOperationLease() {
  Reset();
}

UserModeMappingGenerationOperationLease::
    UserModeMappingGenerationOperationLease(
        UserModeMappingGenerationOperationLease&& other) noexcept
    : token_(std::move(other.token_)) {}

UserModeMappingGenerationOperationLease&
UserModeMappingGenerationOperationLease::operator=(
    UserModeMappingGenerationOperationLease&& other) noexcept {
  if (this != &other) {
    Reset();
    token_ = std::move(other.token_);
  }
  return *this;
}

void UserModeMappingGenerationOperationLease::Reset() noexcept {
  if (token_) {
    token_->ReleaseOperation();
    token_.reset();
  }
}

UserModeManager::UserModeManager(UserModeManagerMode mode) : mode_(mode) {}

UserModeManager::~UserModeManager() {
  PendingMappingRetirements pending_retirements;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < mappings_.size(); ++i) {
      MappingSlot& slot = mappings_[i];
      if (slot.generation_token) {
        pending_retirements[i].mapping = slot.generation_token->handle();
      }
      pending_retirements[i].listener = RetireMapping(slot);
    }
  }
  NotifyMappingRetirements(pending_retirements);
}

bool UserModeManager::TryAdvanceGeneration(MappingSlot& slot) {
  if (slot.generation == std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  ++slot.generation;
  return true;
}

std::shared_ptr<UserModeMappingRetirementListener>
UserModeManager::RetireMapping(MappingSlot& slot) {
  // Mark the mapping retired before clearing its metadata, so readers holding
  // a reference see it.
  std::shared_ptr<UserModeMappingRetirementListener> listener;
  if (slot.generation_token) {
    listener = slot.generation_token->RetireAndTakeRetirementListener();
  }
  slot.occupied = false;
  slot.control_page_physical_address = 0;
  slot.create_time_layout.reset();
  slot.generation_token.reset();
  return listener;
}

void UserModeManager::AdvanceFrameRevision(ActiveFrame& frame) {
  ++frame.revision;
  if (frame.revision == 0) {
    ++frame.revision;
  }
}

bool UserModeManager::IsCurrentMappingLocked(
    UserModeMappingHandle handle) const {
  if (handle.selector >= mappings_.size() || handle.generation == 0) {
    return false;
  }
  const MappingSlot& slot = mappings_[handle.selector];
  return slot.occupied && slot.generation == handle.generation &&
         slot.generation_token && slot.generation_token->is_current() &&
         slot.generation_token->handle() == handle;
}

UserModeCreateMappingResult UserModeManager::CreateMapping(
    uint32_t selector, uint32_t control_page_physical_address) {
  return CreateMapping(selector, control_page_physical_address, std::nullopt);
}

UserModeCreateMappingResult UserModeManager::CreateMapping(
    uint32_t selector, uint32_t control_page_physical_address,
    const UserModeCreateTimeAddressSpaceLayout& create_time_layout) {
  return CreateMapping(
      selector, control_page_physical_address,
      std::optional<UserModeCreateTimeAddressSpaceLayout>(create_time_layout));
}

UserModeCreateMappingResult UserModeManager::CreateMapping(
    uint32_t selector, uint32_t control_page_physical_address,
    std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout) {
  UserModeCreateMappingResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == UserModeManagerMode::kDisabled) {
      result.diagnostic = UserModeManagerDiagnostic::kDisabled;
    } else if (control_page_physical_address == 0 ||
               (control_page_physical_address & 0xFFF) != 0) {
      result.guest_status = X_STATUS_INVALID_PARAMETER;
    } else {
      const bool selector_is_valid = selector < mappings_.size();
      const bool physical_address_is_valid =
          control_page_physical_address < 0x20000000;
      const bool selector_is_duplicate =
          selector_is_valid && mappings_[selector].occupied;
      if (!selector_is_valid || !physical_address_is_valid ||
          selector_is_duplicate) {
        result.guest_status = X_STATUS_ACCESS_DENIED;
      } else {
        MappingSlot& slot = mappings_[selector];
        if (!TryAdvanceGeneration(slot)) {
          // Generations never wrap; an exhausted selector stays exhausted.
          result.diagnostic =
              UserModeManagerDiagnostic::kMappingGenerationExhausted;
        } else {
          const UserModeMappingHandle handle{selector, slot.generation};
          slot.control_page_physical_address = control_page_physical_address;
          slot.create_time_layout = std::move(create_time_layout);
          slot.generation_token =
              std::shared_ptr<UserModeMappingGenerationToken>(
                  new UserModeMappingGenerationToken(
                      handle, slot.control_page_physical_address,
                      slot.create_time_layout));
          slot.occupied = true;
          result.guest_status = X_STATUS_SUCCESS;
          result.handle = handle;
        }
      }
    }
  }
  return result;
}

UserModeManagerDiagnostic UserModeManager::DeleteMapping(
    UserModeMappingHandle handle) {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  PendingMappingRetirement pending_retirement;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == UserModeManagerMode::kDisabled) {
      diagnostic = UserModeManagerDiagnostic::kDisabled;
    } else if (!IsCurrentMappingLocked(handle)) {
      diagnostic = UserModeManagerDiagnostic::kInvalidMappingHandle;
    } else if (mode_ == UserModeManagerMode::kExecutionEnabled &&
               std::any_of(active_frames_.cbegin(), active_frames_.cend(),
                           [handle](const auto& frame) {
                             return frame.second.prepared_entry.mapping ==
                                    handle;
                           })) {
      diagnostic = UserModeManagerDiagnostic::kMappingInUse;
    } else {
      MappingSlot& slot = mappings_[handle.selector];
      pending_retirement.mapping = handle;
      pending_retirement.listener = RetireMapping(slot);
    }
  }
  if (pending_retirement.listener) {
    pending_retirement.listener->OnUserModeMappingRetired(
        pending_retirement.mapping);
  }
  return diagnostic;
}

UserModeBeginEnterResult UserModeManager::BeginEnterLocked(
    const UserModeEnterRequest& request) {
  UserModeBeginEnterResult result;
  if (mode_ == UserModeManagerMode::kDisabled) {
    result.diagnostic = UserModeManagerDiagnostic::kDisabled;
  } else if (request.handler_address == 0 ||
             active_frames_.find(request.thread_id) != active_frames_.cend()) {
    result.guest_status = X_STATUS_NOT_SUPPORTED;
  } else if (!IsCurrentMappingLocked(request.mapping)) {
    result.diagnostic = UserModeManagerDiagnostic::kInvalidMappingHandle;
  } else {
    UserModePreparedEntry prepared_entry;
    prepared_entry.thread_id = request.thread_id;
    prepared_entry.mapping = request.mapping;
    prepared_entry.context_virtual_address = request.context_virtual_address;
    // User code starts on the context's r1; the kernel zeroes r1 only as the
    // hypervisor's Enter selector.
    prepared_entry.context = request.entry_context;
    prepared_entry.handler_address = request.handler_address;
    prepared_entry.save_area_end = request.save_area_end;
    prepared_entry.companion_state = request.companion_state;

    ActiveFrame frame;
    frame.prepared_entry = prepared_entry;
    frame.outer_state = request.outer_state;
    frame.current_user_mode_control = prepared_entry.context.user_mode_control;
    AdvanceFrameRevision(frame);
    active_frames_.emplace(request.thread_id, std::move(frame));
    result.prepared_entry = std::move(prepared_entry);
  }
  return result;
}

UserModeProblemStateSystemCallResult
UserModeManager::DispatchProblemStateSystemCall(uint32_t thread_id,
                                                uint64_t number) {
  UserModeProblemStateSystemCallResult result;
  result.number = number;

  std::lock_guard<std::mutex> lock(mutex_);
  if (mode_ == UserModeManagerMode::kDisabled) {
    result.diagnostic = UserModeManagerDiagnostic::kDisabled;
    return result;
  }

  auto frame_it = active_frames_.find(thread_id);
  if (frame_it == active_frames_.end()) {
    result.diagnostic = UserModeManagerDiagnostic::kNoActiveFrame;
    return result;
  }

  ActiveFrame& frame = frame_it->second;
  if (frame.handler_delivery_pending) {
    result.diagnostic = UserModeManagerDiagnostic::kHandlerAlreadyActive;
    return result;
  }
  AdvanceFrameRevision(frame);
  result.mapping = frame.prepared_entry.mapping;
  result.frame_revision = frame.revision;
  result.previous_user_mode_control = frame.current_user_mode_control;
  result.handler_address = frame.prepared_entry.handler_address;
  result.context_virtual_address = frame.prepared_entry.context_virtual_address;
  result.mapping_is_current =
      IsCurrentMappingLocked(frame.prepared_entry.mapping);
  if (number <= 1) {
    frame.current_user_mode_control =
        (frame.current_user_mode_control & ~uint32_t{1}) |
        static_cast<uint32_t>(number);
    result.action = UserModeProblemStateSystemCallAction::kSwitchSlbHalf;
  } else {
    frame.handler_delivery_pending = true;
    result.action = UserModeProblemStateSystemCallAction::kDeliverToHandler;
  }
  result.current_user_mode_control = frame.current_user_mode_control;
  return result;
}

UserModeHandlerDeliveryResult UserModeManager::PrepareHandlerDelivery(
    uint32_t thread_id) {
  UserModeHandlerDeliveryResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  if (mode_ == UserModeManagerMode::kDisabled) {
    result.diagnostic = UserModeManagerDiagnostic::kDisabled;
    return result;
  }
  auto frame_it = active_frames_.find(thread_id);
  if (frame_it == active_frames_.end()) {
    result.diagnostic = UserModeManagerDiagnostic::kNoActiveFrame;
    return result;
  }
  ActiveFrame& frame = frame_it->second;
  if (frame.handler_delivery_pending) {
    result.diagnostic = UserModeManagerDiagnostic::kHandlerAlreadyActive;
    return result;
  }
  if (!IsCurrentMappingLocked(frame.prepared_entry.mapping)) {
    result.diagnostic = UserModeManagerDiagnostic::kInvalidMappingHandle;
    return result;
  }

  frame.handler_delivery_pending = true;
  AdvanceFrameRevision(frame);
  result.mapping = frame.prepared_entry.mapping;
  result.frame_revision = frame.revision;
  result.user_mode_control = frame.current_user_mode_control;
  result.handler_address = frame.prepared_entry.handler_address;
  result.context_virtual_address = frame.prepared_entry.context_virtual_address;
  result.mapping_is_current = true;
  return result;
}

UserModeHandlerResumeResult UserModeManager::ResumeHandler(
    const UserModeHandlerResumeRequest& request) {
  UserModeHandlerResumeResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  if (mode_ == UserModeManagerMode::kDisabled) {
    result.diagnostic = UserModeManagerDiagnostic::kDisabled;
    return result;
  }
  auto frame_it = active_frames_.find(request.thread_id);
  if (frame_it == active_frames_.end()) {
    result.diagnostic = UserModeManagerDiagnostic::kNoActiveFrame;
    return result;
  }
  ActiveFrame& frame = frame_it->second;
  if (!frame.handler_delivery_pending ||
      frame.prepared_entry.mapping != request.mapping ||
      frame.revision != request.expected_frame_revision) {
    result.diagnostic = UserModeManagerDiagnostic::kStaleFrameRevision;
    return result;
  }
  if (!IsCurrentMappingLocked(request.mapping)) {
    result.diagnostic = UserModeManagerDiagnostic::kInvalidMappingHandle;
    return result;
  }

  result.mapping = request.mapping;
  result.previous_user_mode_control = frame.current_user_mode_control;
  frame.current_user_mode_control = request.user_mode_control;
  frame.handler_delivery_pending = false;
  AdvanceFrameRevision(frame);
  result.frame_revision = frame.revision;
  result.current_user_mode_control = frame.current_user_mode_control;
  result.mapping_is_current = true;
  return result;
}

UserModeBeginEnterResult UserModeManager::BeginEnter(
    const UserModeEnterRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  return BeginEnterLocked(request);
}

UserModeBeginEnterBySelectorResult UserModeManager::BeginEnterBySelector(
    const UserModeEnterBySelectorRequest& request) {
  UserModeBeginEnterBySelectorResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == UserModeManagerMode::kDisabled) {
      result.begin.diagnostic = UserModeManagerDiagnostic::kDisabled;
    } else if (request.handler_address == 0 ||
               active_frames_.find(request.thread_id) !=
                   active_frames_.cend()) {
      result.begin.guest_status = X_STATUS_NOT_SUPPORTED;
    } else if (request.mapping_selector >= mappings_.size() ||
               !mappings_[request.mapping_selector].occupied) {
      result.begin.diagnostic =
          UserModeManagerDiagnostic::kInvalidMappingHandle;
    } else {
      const MappingSlot& slot = mappings_[request.mapping_selector];
      result.resolved_mapping = UserModeMappingSnapshot{
          UserModeMappingHandle{request.mapping_selector, slot.generation},
          slot.control_page_physical_address, slot.create_time_layout,
          slot.generation_token};

      UserModeEnterRequest explicit_request;
      explicit_request.thread_id = request.thread_id;
      explicit_request.mapping = result.resolved_mapping->handle;
      explicit_request.context_virtual_address =
          request.context_virtual_address;
      explicit_request.entry_context = request.entry_context;
      explicit_request.handler_address = request.handler_address;
      explicit_request.save_area_end = request.save_area_end;
      explicit_request.companion_state = request.companion_state;
      explicit_request.outer_state = request.outer_state;
      result.begin = BeginEnterLocked(explicit_request);
    }
  }
  return result;
}

UserModeLeaveResult UserModeManager::Leave(uint32_t thread_id) {
  UserModeLeaveResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == UserModeManagerMode::kDisabled) {
      result.diagnostic = UserModeManagerDiagnostic::kDisabled;
    } else {
      auto frame_it = active_frames_.find(thread_id);
      if (frame_it == active_frames_.end()) {
        result.diagnostic = UserModeManagerDiagnostic::kNoActiveFrame;
      } else {
        result.mapping_was_current =
            IsCurrentMappingLocked(frame_it->second.prepared_entry.mapping);
        result.outer_state = frame_it->second.outer_state;
        active_frames_.erase(frame_it);
      }
    }
  }
  return result;
}

bool UserModeManager::OnThreadDestroyed(uint32_t thread_id) {
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != UserModeManagerMode::kDisabled) {
      removed = active_frames_.erase(thread_id) != 0;
    }
  }
  return removed;
}

UserModeManagerDiagnostic UserModeManager::ResetTitle() {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  PendingMappingRetirements pending_retirements;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == UserModeManagerMode::kDisabled) {
      diagnostic = UserModeManagerDiagnostic::kDisabled;
    } else {
      for (size_t i = 0; i < mappings_.size(); ++i) {
        MappingSlot& slot = mappings_[i];
        if (slot.generation_token) {
          pending_retirements[i].mapping = slot.generation_token->handle();
        }
        pending_retirements[i].listener = RetireMapping(slot);
      }
      active_frames_.clear();
    }
  }
  NotifyMappingRetirements(pending_retirements);
  return diagnostic;
}

UserModeSaveStatePreflightResult UserModeManager::CheckSaveStatePreconditions()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_frames_.empty()) {
    return UserModeSaveStatePreflightResult::kActiveFrames;
  }
  if (std::any_of(mappings_.cbegin(), mappings_.cend(),
                  [](const MappingSlot& slot) { return slot.occupied; })) {
    return UserModeSaveStatePreflightResult::kMappingsPresent;
  }
  return UserModeSaveStatePreflightResult::kReady;
}

std::optional<UserModeMappingSnapshot> UserModeManager::FindMapping(
    UserModeMappingHandle handle) const {
  std::optional<UserModeMappingSnapshot> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != UserModeManagerMode::kDisabled &&
        IsCurrentMappingLocked(handle)) {
      result = UserModeMappingSnapshot{
          handle, mappings_[handle.selector].control_page_physical_address,
          mappings_[handle.selector].create_time_layout,
          mappings_[handle.selector].generation_token};
    }
  }
  return result;
}

std::optional<UserModeMappingSnapshot> UserModeManager::FindMappingBySelector(
    uint32_t selector) const {
  std::optional<UserModeMappingSnapshot> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != UserModeManagerMode::kDisabled &&
        selector < mappings_.size()) {
      const MappingSlot& slot = mappings_[selector];
      if (slot.occupied) {
        result = UserModeMappingSnapshot{
            UserModeMappingHandle{selector, slot.generation},
            slot.control_page_physical_address, slot.create_time_layout,
            slot.generation_token};
      }
    }
  }
  return result;
}

std::optional<UserModeActiveFrameSnapshot> UserModeManager::FindActiveFrame(
    uint32_t thread_id) const {
  std::optional<UserModeActiveFrameSnapshot> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != UserModeManagerMode::kDisabled) {
      auto frame_it = active_frames_.find(thread_id);
      if (frame_it != active_frames_.cend()) {
        result = UserModeActiveFrameSnapshot{
            frame_it->second.prepared_entry,
            frame_it->second.outer_state,
            frame_it->second.current_user_mode_control,
            IsCurrentMappingLocked(frame_it->second.prepared_entry.mapping),
            frame_it->second.revision,
            frame_it->second.handler_delivery_pending};
      }
    }
  }
  return result;
}

}  // namespace xe::kernel

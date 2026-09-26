/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_USER_MODE_MANAGER_H_
#define XENIA_KERNEL_USER_MODE_MANAGER_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "xenia/kernel/user_mode_address_space.h"
#include "xenia/kernel/user_mode_context.h"
#include "xenia/xbox.h"

namespace xe::kernel {

class UserModeGuestCodeDomainCoordinator;
class UserModeGuestDataAccessor;
class UserModeMappingGenerationOperationLease;

// kSyntheticStateOnly runs the state machine without guest execution;
// kExecutionEnabled is the kernel runtime's live mode.
enum class UserModeManagerMode {
  kDisabled,
  kSyntheticStateOnly,
  kExecutionEnabled,
};

// Internal results; guest statuses are reported separately.
enum class UserModeManagerDiagnostic {
  kNone,
  kDisabled,
  kInvalidMappingHandle,
  kMappingInUse,
  kNoActiveFrame,
  kHandlerAlreadyActive,
  kStaleFrameRevision,
  kMappingGenerationExhausted,
};

// Whether user-mode state blocks a save: mappings and frames aren't saved.
enum class UserModeSaveStatePreflightResult {
  kReady,
  kMappingsPresent,
  kActiveFrames,
};

struct UserModeMappingHandle {
  uint32_t selector = 0;
  uint64_t generation = 0;

  explicit operator bool() const { return generation != 0; }
};

inline bool operator==(const UserModeMappingHandle& left,
                       const UserModeMappingHandle& right) {
  return left.selector == right.selector && left.generation == right.generation;
}

inline bool operator!=(const UserModeMappingHandle& left,
                       const UserModeMappingHandle& right) {
  return !(left == right);
}

// Notified once when a mapping generation retires, outside the manager's
// lock. Must not throw.
class UserModeMappingRetirementListener {
 public:
  virtual ~UserModeMappingRetirementListener() = default;
  virtual void OnUserModeMappingRetired(
      UserModeMappingHandle mapping) noexcept = 0;
};

enum class UserModeMappingRetirementListenerAttachmentDiagnostic {
  kNone,
  kInvalidListener,
  kMappingNotCurrent,
  kListenerAlreadyAttached,
};

// Whether one mapping generation is current. Readers hold it, so retirement
// invalidates their snapshots without freeing what they use. A retired
// generation never becomes current again.
class UserModeMappingGenerationToken final {
 public:
  UserModeMappingGenerationToken(const UserModeMappingGenerationToken&) =
      delete;
  UserModeMappingGenerationToken& operator=(
      const UserModeMappingGenerationToken&) = delete;

  UserModeMappingHandle handle() const { return handle_; }
  uint32_t control_page_physical_address() const {
    return control_page_physical_address_;
  }
  const std::optional<UserModeCreateTimeAddressSpaceLayout>&
  create_time_layout() const {
    return create_time_layout_;
  }
  bool is_current() const { return current_.load(std::memory_order_acquire); }

 private:
  friend class UserModeGuestCodeDomainCoordinator;
  friend class UserModeMappingGenerationOperationLease;
  friend class UserModeManager;

  UserModeMappingGenerationToken(
      UserModeMappingHandle handle, uint32_t control_page_physical_address,
      std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout)
      : handle_(handle),
        control_page_physical_address_(control_page_physical_address),
        create_time_layout_(std::move(create_time_layout)) {}

  // Attaches the generation's one listener, held weakly; a later retirement
  // notifies it.
  UserModeMappingRetirementListenerAttachmentDiagnostic
  AttachRetirementListener(
      const std::weak_ptr<UserModeMappingRetirementListener>& listener) const;

  std::shared_ptr<UserModeMappingRetirementListener>
  RetireAndTakeRetirementListener();
  bool TryAcquireOperation() const;
  void ReleaseOperation() const;

  const UserModeMappingHandle handle_;
  const uint32_t control_page_physical_address_;
  const std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout_;
  std::atomic<bool> current_{true};
  mutable std::mutex lifecycle_mutex_;
  mutable std::condition_variable lifecycle_condition_;
  mutable size_t active_operation_count_ = 0;
  mutable std::weak_ptr<UserModeMappingRetirementListener> retirement_listener_;
  mutable bool retirement_listener_attached_ = false;
};

// Held by a host operation on a mapping; retirement marks the generation
// non-current, then waits for its leases. A holder must not call
// UserModeManager or external callbacks. The checked data accessor takes it
// under the global lock.
class UserModeMappingGenerationOperationLease final {
 public:
  UserModeMappingGenerationOperationLease() = default;
  ~UserModeMappingGenerationOperationLease();

  UserModeMappingGenerationOperationLease(
      UserModeMappingGenerationOperationLease&& other) noexcept;
  UserModeMappingGenerationOperationLease& operator=(
      UserModeMappingGenerationOperationLease&& other) noexcept;

  UserModeMappingGenerationOperationLease(
      const UserModeMappingGenerationOperationLease&) = delete;
  UserModeMappingGenerationOperationLease& operator=(
      const UserModeMappingGenerationOperationLease&) = delete;

  explicit operator bool() const { return static_cast<bool>(token_); }

 private:
  friend class UserModeGuestDataAccessor;

  explicit UserModeMappingGenerationOperationLease(
      std::shared_ptr<const UserModeMappingGenerationToken> token);
  void Reset() noexcept;

  std::shared_ptr<const UserModeMappingGenerationToken> token_;
};

struct UserModeMappingSnapshot {
  UserModeMappingHandle handle;
  uint32_t control_page_physical_address = 0;

  // The Create-time segment layout of a mapping made from a guest control
  // page.
  std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout;

  // Set by the manager; checked readers reject a snapshot without one.
  std::shared_ptr<const UserModeMappingGenerationToken> generation_token;
};

struct UserModeCreateMappingResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  std::optional<X_STATUS> guest_status;
  std::optional<UserModeMappingHandle> handle;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone &&
           guest_status.has_value() && XSUCCEEDED(*guest_status) &&
           handle.has_value();
  }
};

// The integer state of the outer (kernel) context saved at Enter, without
// floating-point or vector state.
struct UserModeOuterIntegerState {
  std::array<uint64_t, 32> gpr{};
  uint64_t link_register = 0;
  uint64_t count_register = 0;
  uint64_t machine_state_register = 0;
  uint32_t condition_register = 0;
  uint8_t xer_ca = 0;
  uint8_t xer_ov = 0;
  uint8_t xer_so = 0;
};

struct UserModeEnterRequest {
  uint32_t thread_id = 0;
  UserModeMappingHandle mapping;
  uint32_t context_virtual_address = 0;
  UserModeIntegerContext entry_context;
  uint32_t handler_address = 0;
  uint32_t save_area_end = 0;
  uint32_t companion_state = 0;
  UserModeOuterIntegerState outer_state;
};

// UserModeEnterRequest by selector (0 or 1); the manager resolves its current
// mapping when it claims the frame.
struct UserModeEnterBySelectorRequest {
  uint32_t thread_id = 0;
  uint32_t mapping_selector = 0;
  uint32_t context_virtual_address = 0;
  UserModeIntegerContext entry_context;
  uint32_t handler_address = 0;
  uint32_t save_area_end = 0;
  uint32_t companion_state = 0;
  UserModeOuterIntegerState outer_state;
};

struct UserModePreparedEntry {
  uint32_t thread_id = 0;
  UserModeMappingHandle mapping;
  uint32_t context_virtual_address = 0;
  UserModeIntegerContext context;
  uint32_t handler_address = 0;
  uint32_t save_area_end = 0;
  uint32_t companion_state = 0;
};

struct UserModeBeginEnterResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;

  // Set only for a rejection the guest sees.
  std::optional<X_STATUS> guest_status;
  std::optional<UserModePreparedEntry> prepared_entry;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone &&
           !guest_status.has_value() && prepared_entry.has_value();
  }
};

struct UserModeBeginEnterBySelectorResult {
  UserModeBeginEnterResult begin;
  std::optional<UserModeMappingSnapshot> resolved_mapping;

  explicit operator bool() const {
    return static_cast<bool>(begin) && resolved_mapping.has_value();
  }
};

struct UserModeActiveFrameSnapshot {
  UserModePreparedEntry prepared_entry;
  UserModeOuterIntegerState outer_state;
  uint32_t current_user_mode_control = 0;
  bool mapping_is_current = false;
  uint64_t frame_revision = 0;
  bool handler_delivery_pending = false;
};

// A problem-state sc: values 0 and 1 set UserModeControl bit 0, selecting an
// SLB half; any other value goes to the custom handler.
enum class UserModeProblemStateSystemCallAction {
  kSwitchSlbHalf,
  kDeliverToHandler,
};

struct UserModeProblemStateSystemCallResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  std::optional<UserModeProblemStateSystemCallAction> action;

  // Set on success; with the frame revision it identifies the transition.
  UserModeMappingHandle mapping;
  uint64_t frame_revision = 0;
  uint64_t number = 0;
  uint32_t previous_user_mode_control = 0;
  uint32_t current_user_mode_control = 0;
  uint32_t handler_address = 0;
  uint32_t context_virtual_address = 0;
  bool mapping_is_current = false;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone && action.has_value();
  }
};

struct UserModeHandlerDeliveryResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  UserModeMappingHandle mapping;
  uint64_t frame_revision = 0;
  uint32_t user_mode_control = 0;
  uint32_t handler_address = 0;
  uint32_t context_virtual_address = 0;
  bool mapping_is_current = false;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone &&
           static_cast<bool>(mapping) && frame_revision != 0 &&
           handler_address != 0 && context_virtual_address != 0 &&
           mapping_is_current;
  }
};

struct UserModeHandlerResumeRequest {
  uint32_t thread_id = 0;
  UserModeMappingHandle mapping;
  uint64_t expected_frame_revision = 0;
  uint32_t user_mode_control = 0;
};

struct UserModeHandlerResumeResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  UserModeMappingHandle mapping;
  uint64_t frame_revision = 0;
  uint32_t previous_user_mode_control = 0;
  uint32_t current_user_mode_control = 0;
  bool mapping_is_current = false;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone &&
           static_cast<bool>(mapping) && frame_revision != 0 &&
           mapping_is_current;
  }
};

struct UserModeLeaveResult {
  UserModeManagerDiagnostic diagnostic = UserModeManagerDiagnostic::kNone;
  std::optional<UserModeOuterIntegerState> outer_state;
  bool mapping_was_current = false;

  explicit operator bool() const {
    return diagnostic == UserModeManagerDiagnostic::kNone &&
           outer_state.has_value();
  }
};

class UserModeManager {
 public:
  explicit UserModeManager(
      UserModeManagerMode mode = UserModeManagerMode::kDisabled);
  ~UserModeManager();

  UserModeManager(const UserModeManager&) = delete;
  UserModeManager& operator=(const UserModeManager&) = delete;

  UserModeManagerMode mode() const { return mode_; }

  // Creates a mapping without a layout. A zero or unaligned physical page is
  // INVALID_PARAMETER; then a bad selector, physical range or occupied
  // selector is ACCESS_DENIED. Guest requests use the overload below.
  UserModeCreateMappingResult CreateMapping(
      uint32_t selector, uint32_t control_page_physical_address);

  // The same, with the Create-time segment layout.
  UserModeCreateMappingResult CreateMapping(
      uint32_t selector, uint32_t control_page_physical_address,
      const UserModeCreateTimeAddressSpaceLayout& create_time_layout);

  // In execution mode, refuses a mapping an active frame still uses.
  UserModeManagerDiagnostic DeleteMapping(UserModeMappingHandle handle);

  // Copies both input states; neither they nor a PPCContext are modified.
  UserModeBeginEnterResult BeginEnter(const UserModeEnterRequest& request);

  // Resolves the selector's current mapping and begins the enter. A zero
  // handler or an already active thread is NOT_SUPPORTED, checked before the
  // mapping.
  UserModeBeginEnterBySelectorResult BeginEnterBySelector(
      const UserModeEnterBySelectorRequest& request);

  // Updates the manager's state for a problem-state sc; the caller performs
  // the switch or the handler delivery.
  UserModeProblemStateSystemCallResult DispatchProblemStateSystemCall(
      uint32_t thread_id, uint64_t number);

  // Claims the custom-handler path for a checked-memory fault. Resuming needs
  // the returned revision; a second delivery can't overwrite a pending one.
  UserModeHandlerDeliveryResult PrepareHandlerDelivery(uint32_t thread_id);

  // Completes a pending handler callback and sets the (possibly edited)
  // UserModeControl of the resumed context, after revalidating the mapping
  // generation and frame revision.
  UserModeHandlerResumeResult ResumeHandler(
      const UserModeHandlerResumeRequest& request);

  // Pops thread_id's frame and returns its outer state, even if the mapping
  // was deleted.
  UserModeLeaveResult Leave(uint32_t thread_id);

  // Drops the thread's frames; idempotent.
  bool OnThreadDestroyed(uint32_t thread_id);
  UserModeManagerDiagnostic ResetTitle();

  UserModeSaveStatePreflightResult CheckSaveStatePreconditions() const;

  // Retail user mode has one mapping slot per selector.
  static constexpr size_t kMappingSelectorCount = 2;

  std::optional<UserModeMappingSnapshot> FindMapping(
      UserModeMappingHandle handle) const;

  // The current mapping of a selector. The handle may go stale at once;
  // manager operations revalidate it.
  std::optional<UserModeMappingSnapshot> FindMappingBySelector(
      uint32_t selector) const;
  std::optional<UserModeActiveFrameSnapshot> FindActiveFrame(
      uint32_t thread_id) const;

 private:
  struct MappingSlot {
    uint64_t generation = 0;
    uint32_t control_page_physical_address = 0;
    std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout;
    std::shared_ptr<UserModeMappingGenerationToken> generation_token;
    bool occupied = false;
  };

  struct ActiveFrame {
    UserModePreparedEntry prepared_entry;
    UserModeOuterIntegerState outer_state;
    uint32_t current_user_mode_control = 0;
    uint64_t revision = 0;
    bool handler_delivery_pending = false;
  };

  static bool TryAdvanceGeneration(MappingSlot& slot);
  static std::shared_ptr<UserModeMappingRetirementListener> RetireMapping(
      MappingSlot& slot);
  static void AdvanceFrameRevision(ActiveFrame& frame);
  UserModeCreateMappingResult CreateMapping(
      uint32_t selector, uint32_t control_page_physical_address,
      std::optional<UserModeCreateTimeAddressSpaceLayout> create_time_layout);
  bool IsCurrentMappingLocked(UserModeMappingHandle handle) const;
  UserModeBeginEnterResult BeginEnterLocked(
      const UserModeEnterRequest& request);

  const UserModeManagerMode mode_;
  mutable std::mutex mutex_;
  std::array<MappingSlot, kMappingSelectorCount> mappings_{};
  std::unordered_map<uint32_t, ActiveFrame> active_frames_;
};

}  // namespace xe::kernel

#endif  // XENIA_KERNEL_USER_MODE_MANAGER_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_PPC_UNWIND_H_
#define XENIA_KERNEL_UTIL_PPC_UNWIND_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Helpers for Xbox 360 structured exception handling: the CONTEXT record, the
// compressed PowerPC .pdata format, virtual unwinding of the prologs the Xenon
// compiler emits, and the __C_specific_handler scope table. Guest memory is
// read through GuestMemoryReader.

namespace xe {
namespace kernel {
namespace util {

// ---------------------------------------------------------------------------
// CONTEXT (Xenon winnt.h). Only the fields SEH captures and restores are
// interpreted; UserModeControl (0x228) and the vector registers that follow
// (0x230 onward) are left alone.
inline constexpr uint32_t kPpcContextSize = 0xA40;
inline constexpr uint32_t kPpcContextFlagsOffset = 0x000;
inline constexpr uint32_t kPpcContextMsrOffset = 0x004;
inline constexpr uint32_t kPpcContextIarOffset = 0x008;
inline constexpr uint32_t kPpcContextLrOffset = 0x00C;
inline constexpr uint32_t kPpcContextCtrOffset = 0x010;  // 64-bit
inline constexpr uint32_t kPpcContextGprOffset = 0x018;  // 32 x 64-bit
inline constexpr uint32_t kPpcContextCrOffset = 0x118;
inline constexpr uint32_t kPpcContextXerOffset = 0x11C;
// A double-sized slot: mffs/stfd leave FPSCR in its low word.
inline constexpr uint32_t kPpcContextFpscrOffset = 0x120;
inline constexpr uint32_t kPpcContextFpscrValueOffset = 0x124;
inline constexpr uint32_t kPpcContextFprOffset = 0x128;  // 32 x 64-bit
inline constexpr uint32_t kPpcContextUserModeControlOffset = 0x228;
// Bytes EncodePpcContext writes and DecodePpcContext reads.
inline constexpr uint32_t kPpcContextEncodedSize = 0x228;

inline constexpr uint32_t kPpcContextControl = 0x1;        // MSR IAR LR CTR
inline constexpr uint32_t kPpcContextFloatingPoint = 0x2;  // FPSCR f0-f31
inline constexpr uint32_t kPpcContextInteger = 0x4;        // r0-r31 CR XER
inline constexpr uint32_t kPpcContextFull =
    kPpcContextControl | kPpcContextFloatingPoint | kPpcContextInteger;

// The register state SEH captures, unwinds and restores. FPRs are kept as raw
// IEEE bits.
struct PpcRegisterState {
  uint32_t context_flags = 0;
  uint32_t msr = 0;
  uint32_t iar = 0;
  uint32_t lr = 0;
  uint64_t ctr = 0;
  std::array<uint64_t, 32> gpr{};
  uint32_t cr = 0;
  uint32_t xer = 0;
  uint32_t fpscr = 0;
  std::array<uint64_t, 32> fpr{};
};

// Writes every PpcRegisterState field (whatever context_flags says) into a
// big-endian CONTEXT of at least kPpcContextEncodedSize bytes. Bytes from
// kPpcContextEncodedSize on are untouched. Returns false if bytes is short.
bool EncodePpcContext(const PpcRegisterState& state, std::span<uint8_t> bytes);
bool DecodePpcContext(std::span<const uint8_t> bytes, PpcRegisterState* state);

// ---------------------------------------------------------------------------
// EXCEPTION_RECORD flags and handler dispositions.
inline constexpr uint32_t kExceptionNoncontinuable = 0x01;
inline constexpr uint32_t kExceptionUnwinding = 0x02;
inline constexpr uint32_t kExceptionExitUnwind = 0x04;
inline constexpr uint32_t kExceptionStackInvalid = 0x08;
inline constexpr uint32_t kExceptionNestedCall = 0x10;
inline constexpr uint32_t kExceptionTargetUnwind = 0x20;
inline constexpr uint32_t kExceptionCollidedUnwind = 0x40;
inline constexpr uint32_t kExceptionUnwind =
    kExceptionUnwinding | kExceptionExitUnwind | kExceptionTargetUnwind |
    kExceptionCollidedUnwind;

inline constexpr uint32_t kExceptionContinueExecution = 0;
inline constexpr uint32_t kExceptionContinueSearch = 1;
inline constexpr uint32_t kExceptionNestedException = 2;
inline constexpr uint32_t kExceptionCollidedUnwindDisposition = 3;

inline constexpr uint32_t kStatusUnwind = 0xC0000027;

// EXCEPTION_RECORD (32-bit): code, flags, record, address, parameter count.
inline constexpr uint32_t kExceptionRecordCodeOffset = 0x00;
inline constexpr uint32_t kExceptionRecordFlagsOffset = 0x04;
inline constexpr uint32_t kExceptionRecordAddressOffset = 0x0C;
inline constexpr uint32_t kExceptionRecordSize = 0x50;

// DISPATCHER_CONTEXT (PowerPC): ControlPc, FunctionEntry, EstablisherFrame,
// ContextRecord. The C++ frame handler reads FunctionEntry at +4 and the
// handler data at FunctionEntry->BeginAddress - 4.
inline constexpr uint32_t kDispatcherContextControlPcOffset = 0x0;
inline constexpr uint32_t kDispatcherContextFunctionEntryOffset = 0x4;
inline constexpr uint32_t kDispatcherContextEstablisherFrameOffset = 0x8;
inline constexpr uint32_t kDispatcherContextContextRecordOffset = 0xC;
inline constexpr uint32_t kDispatcherContextSize = 0x10;

// ---------------------------------------------------------------------------
// Big-endian guest memory access. Implementations return false for an address
// that cannot be read.
class GuestMemoryReader {
 public:
  virtual ~GuestMemoryReader() = default;
  virtual bool Read32(uint32_t address, uint32_t* value) const = 0;
  virtual bool Read64(uint32_t address, uint64_t* value) const = 0;
};

// ---------------------------------------------------------------------------
// .pdata. Each 8-byte entry is BeginAddress and a packed word: PrologLength
// (bits 0-7, instructions), FunctionLength (bits 8-29, instructions),
// ThirtyTwoBit (bit 30) and ExceptionFlag (bit 31). With both flag bits set
// the handler and its data are the two words before BeginAddress. With
// ThirtyTwoBit clear the entry describes a register save (ExceptionFlag
// clear) or restore (set) millicode routine (__savegprlr, __restfpr, ...).
struct PpcRuntimeFunction {
  uint32_t entry_address = 0;  // guest address of the .pdata entry
  uint32_t begin_address = 0;
  uint32_t packed = 0;

  uint32_t prolog_length() const { return packed & 0xFF; }
  uint32_t function_length() const { return (packed >> 8) & 0x3FFFFF; }
  bool is_32bit() const { return ((packed >> 30) & 1) != 0; }
  bool exception_flag() const { return (packed >> 31) != 0; }
  bool has_handler() const { return is_32bit() && exception_flag(); }
  bool is_millicode() const { return !is_32bit(); }
  uint32_t prolog_end() const { return begin_address + prolog_length() * 4; }
  uint32_t end_address() const { return begin_address + function_length() * 4; }
  uint32_t handler_address() const { return begin_address - 8; }
  uint32_t handler_data_address() const { return begin_address - 4; }
};

struct PpcGuestRange {
  uint32_t address = 0;
  uint32_t size = 0;
};

// The exception directory (IMAGE_DIRECTORY_ENTRY_EXCEPTION) of the
// little-endian PE32 headers a loaded XEX image keeps at its base.
std::optional<PpcGuestRange> FindPeExceptionDirectory(
    const GuestMemoryReader& reader, uint32_t image_base);

// Binary search of a .pdata table sorted by BeginAddress; trailing zero
// entries are ignored.
std::optional<PpcRuntimeFunction> LookupPpcRuntimeFunction(
    const GuestMemoryReader& reader, PpcGuestRange table, uint32_t pc);

// ---------------------------------------------------------------------------
// Prolog simulation. The prolog is executed symbolically from BeginAddress,
// following calls into the register save millicode (to its blr), to learn
// where the entry values of r1, LR, CR, r14-r31 and f14-f31 went.
struct PpcPrologValue {
  enum class Kind : uint8_t {
    kUnknown,
    kStackRelative,  // entry r1 + value
    kConstant,       // value
    kEntryGpr,       // the entry value of GPR index
    kEntryFpr,       // the entry value of FPR index
    kEntryLr,
    kEntryCr,
  };
  Kind kind = Kind::kUnknown;
  uint8_t index = 0;
  int64_t value = 0;

  bool operator==(const PpcPrologValue& other) const {
    return kind == other.kind && index == other.index && value == other.value;
  }
};

struct PpcPrologSaveSlot {
  PpcPrologValue saved;
  int32_t offset = 0;  // from the entry r1
  uint8_t size = 0;    // 4 or 8
};

struct PpcPrologState {
  std::array<PpcPrologValue, 32> gpr;
  std::array<PpcPrologValue, 32> fpr;
  PpcPrologValue lr;
  PpcPrologValue cr;
  static constexpr size_t kMaxSlots = 96;
  std::array<PpcPrologSaveSlot, kMaxSlots> slots;
  uint32_t slot_count = 0;
  // r1 moved by an amount the simulation could not compute (stwux with a
  // run-time size): the entry r1 is then taken from the back chain.
  bool dynamic_frame = false;
  // A prolog instruction the simulation does not model was skipped.
  bool saw_unknown_instruction = false;

  const PpcPrologSaveSlot* FindSlot(const PpcPrologValue& saved) const;
};

// Simulates the instructions in [begin_address, stop_address). Returns false
// only when an instruction cannot be read.
bool SimulatePpcProlog(const GuestMemoryReader& reader, uint32_t begin_address,
                       uint32_t stop_address, PpcPrologState* state);

struct PpcUnwindResult {
  // The caller's registers: r1, LR, CR, r14-r31 and f14-f31 restored, IAR set
  // to the return address; everything else as in the input.
  PpcRegisterState caller;
  // r1 on entry to the function (the caller's r1). Funclets receive it in r12
  // and C++ EH keeps state at EstablisherFrame + 4.
  uint32_t establisher_frame = 0;
  // control_pc lies past the prolog, where the language handler applies.
  bool in_function = false;
};

// Virtually unwinds one frame. control_pc is the instruction executing in
// the function (a call instruction for a caller frame). Epilogues are not
// recognised: control_pc must be in the prolog or the body.
bool PpcVirtualUnwind(const GuestMemoryReader& reader,
                      const PpcRuntimeFunction& function, uint32_t control_pc,
                      const PpcRegisterState& state, PpcUnwindResult* result);

// ---------------------------------------------------------------------------
// __C_specific_handler scope table (absolute addresses): a count, then
// {BeginAddress, EndAddress, HandlerAddress, JumpTarget} records, innermost
// first. JumpTarget 0 marks a __finally whose HandlerAddress is the
// termination handler; otherwise HandlerAddress is the __except filter (or 1,
// EXCEPTION_EXECUTE_HANDLER) and JumpTarget the __except block.
struct CScopeRecord {
  uint32_t begin_address = 0;
  uint32_t end_address = 0;
  uint32_t handler_address = 0;
  uint32_t jump_target = 0;
};
inline constexpr uint32_t kCScopeExecuteHandlerFilter = 1;
inline constexpr uint32_t kMaxCScopeRecords = 0x10000;

std::optional<uint32_t> ReadCScopeCount(const GuestMemoryReader& reader,
                                        uint32_t scope_table);
bool ReadCScopeRecord(const GuestMemoryReader& reader, uint32_t scope_table,
                      uint32_t index, CScopeRecord* record);

enum class CScopeDispatchAction {
  kContinueSearch,
  kContinueExecution,
  kExecuteHandler,
  kBadTable,
};
struct CScopeDispatchDecision {
  CScopeDispatchAction action = CScopeDispatchAction::kContinueSearch;
  CScopeRecord scope;
};
// Evaluates a filter (a guest call); returns its EXCEPTION_* value.
using CScopeFilterCallback = int32_t (*)(void* opaque,
                                         const CScopeRecord& scope);
// The exception dispatch half of __C_specific_handler: evaluates, in order,
// the filters of the __except scopes containing control_pc. A negative filter
// value dismisses the exception, a positive one selects the scope's handler.
CScopeDispatchDecision DispatchCScopeTable(const GuestMemoryReader& reader,
                                           uint32_t scope_table,
                                           uint32_t control_pc,
                                           CScopeFilterCallback filter,
                                           void* opaque);

// Runs a termination handler (a guest call).
using CScopeTerminationCallback = void (*)(void* opaque,
                                           const CScopeRecord& scope);
// The unwind half: calls, in order, the termination handlers of the scopes
// containing control_pc, stopping at a scope that also contains target_pc
// (end inclusive: a leave) or whose __except block is target_pc. Returns
// false for an unreadable table.
bool UnwindCScopeTable(const GuestMemoryReader& reader, uint32_t scope_table,
                       uint32_t control_pc, uint32_t target_pc,
                       CScopeTerminationCallback terminate, void* opaque);

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_PPC_UNWIND_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/ppc_unwind.h"

#include <algorithm>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace util {

namespace {

// ---------------------------------------------------------------------------
// CONTEXT codec helpers. The callers check the span size.
uint32_t ReadBigEndian32(std::span<const uint8_t> bytes, size_t offset) {
  return xe::load_and_swap<uint32_t>(bytes.data() + offset);
}

uint64_t ReadBigEndian64(std::span<const uint8_t> bytes, size_t offset) {
  return xe::load_and_swap<uint64_t>(bytes.data() + offset);
}

void WriteBigEndian32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  xe::store_and_swap<uint32_t>(bytes.data() + offset, value);
}

void WriteBigEndian64(std::span<uint8_t> bytes, size_t offset, uint64_t value) {
  xe::store_and_swap<uint64_t>(bytes.data() + offset, value);
}

// ---------------------------------------------------------------------------
// Instruction fields (IBM bit numbering: bit 0 is the most significant).
constexpr uint32_t kBlr = 0x4E800020;
// Bounds the walk through a register save routine; __savevmx_64 is the
// longest the compiler links (about 130 instructions).
constexpr uint32_t kMaxMillicodeInstructions = 512;

uint32_t Opcode(uint32_t word) { return word >> 26; }
uint32_t FieldD(uint32_t word) { return (word >> 21) & 31; }  // rD, rS, frS
uint32_t FieldA(uint32_t word) { return (word >> 16) & 31; }
uint32_t FieldB(uint32_t word) { return (word >> 11) & 31; }
uint32_t ExtendedOpcode(uint32_t word) { return (word >> 1) & 0x3FF; }
uint32_t SprNumber(uint32_t word) {
  return ((word >> 16) & 0x1F) | (((word >> 11) & 0x1F) << 5);
}
int64_t SignExtend16(uint32_t value) {
  return static_cast<int16_t>(static_cast<uint16_t>(value));
}

constexpr uint32_t kSprLr = 8;

// X-form (primary opcode 31) groups by which GPR, if any, they write.
bool IsStoreIndexed(uint32_t xo) {
  switch (xo) {
    case 135:  // stvebx
    case 149:  // stdx
    case 150:  // stwcx.
    case 151:  // stwx
    case 167:  // stvehx
    case 181:  // stdux
    case 183:  // stwux
    case 199:  // stvewx
    case 214:  // stdcx.
    case 215:  // stbx
    case 231:  // stvx
    case 247:  // stbux
    case 407:  // sthx
    case 439:  // sthux
    case 487:  // stvxl
    case 647:  // stvlx
    case 660:  // stdbrx
    case 661:  // stswx
    case 662:  // stwbrx
    case 663:  // stfsx
    case 679:  // stvrx
    case 695:  // stfsux
    case 725:  // stswi
    case 727:  // stfdx
    case 759:  // stfdux
    case 903:  // stvlxl
    case 918:  // sthbrx
    case 935:  // stvrxl
    case 983:  // stfiwx
      return true;
    default:
      return false;
  }
}

bool IsStoreIndexedWithUpdate(uint32_t xo) {
  return xo == 181 || xo == 183 || xo == 247 || xo == 439 || xo == 695 ||
         xo == 759;
}

// Destination rA.
bool IsLogicalIndexed(uint32_t xo) {
  switch (xo) {
    case 24:   // slw
    case 26:   // cntlzw
    case 27:   // sld
    case 28:   // and
    case 58:   // cntlzd
    case 60:   // andc
    case 122:  // popcntb
    case 124:  // nor
    case 284:  // eqv
    case 316:  // xor
    case 412:  // orc
    case 476:  // nand
    case 536:  // srw
    case 539:  // srd
    case 792:  // sraw
    case 794:  // srad
    case 824:  // srawi
    case 826:  // sradi (XS-form, sh5 = 0)
    case 827:  // sradi (XS-form, sh5 = 1)
    case 922:  // extsh
    case 954:  // extsb
    case 986:  // extsw
      return true;
    default:
      return false;
  }
}

// Write rD and rA.
bool IsLoadIndexedWithUpdate(uint32_t xo) {
  return xo == 53 || xo == 55 || xo == 119 || xo == 311 || xo == 373 ||
         xo == 375;
}

// Write an FPR or a vector register, or nothing that is tracked.
bool HasNoTrackedGprResult(uint32_t xo) {
  switch (xo) {
    case 0:     // cmp
    case 4:     // tw
    case 6:     // lvsl
    case 7:     // lvebx
    case 32:    // cmpl
    case 38:    // lvsr
    case 39:    // lvehx
    case 54:    // dcbst
    case 68:    // td
    case 71:    // lvewx
    case 86:    // dcbf
    case 103:   // lvx
    case 144:   // mtcrf
    case 146:   // mtmsr
    case 178:   // mtmsrd
    case 210:   // mtsr
    case 242:   // mtsrin
    case 246:   // dcbtst
    case 278:   // dcbt
    case 306:   // tlbie
    case 359:   // lvxl
    case 402:   // slbmte
    case 470:   // dcbi
    case 519:   // lvlx
    case 551:   // lvrx
    case 566:   // tlbsync
    case 598:   // sync
    case 775:   // lvlxl
    case 807:   // lvrxl
    case 854:   // eieio
    case 982:   // icbi
    case 1014:  // dcbz
      return true;
    default:
      return false;
  }
}

bool IsFloatLoadIndexed(uint32_t xo, bool* update) {
  switch (xo) {
    case 535:  // lfsx
    case 599:  // lfdx
      *update = false;
      return true;
    case 567:  // lfsux
    case 631:  // lfdux
      *update = true;
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Symbolic values.
using Kind = PpcPrologValue::Kind;

PpcPrologValue MakeValue(Kind kind, uint8_t index = 0, int64_t value = 0) {
  PpcPrologValue result;
  result.kind = kind;
  result.index = index;
  result.value = value;
  return result;
}

PpcPrologValue Unknown() { return MakeValue(Kind::kUnknown); }
PpcPrologValue StackRelative(int64_t offset) {
  return MakeValue(Kind::kStackRelative, 0, offset);
}
PpcPrologValue Constant(int64_t value) {
  return MakeValue(Kind::kConstant, 0, value);
}

bool IsEntryValue(const PpcPrologValue& value) {
  return value.kind == Kind::kEntryGpr || value.kind == Kind::kEntryFpr ||
         value.kind == Kind::kEntryLr || value.kind == Kind::kEntryCr;
}

PpcPrologValue Add(const PpcPrologValue& a, const PpcPrologValue& b) {
  if (a.kind == Kind::kStackRelative && b.kind == Kind::kConstant) {
    return StackRelative(a.value + b.value);
  }
  if (a.kind == Kind::kConstant && b.kind == Kind::kStackRelative) {
    return StackRelative(a.value + b.value);
  }
  if (a.kind == Kind::kConstant && b.kind == Kind::kConstant) {
    return Constant(a.value + b.value);
  }
  return Unknown();
}

class PrologSimulator {
 public:
  enum class StepResult { kContinue, kStop, kReadError };

  PrologSimulator(const GuestMemoryReader& reader, PpcPrologState* state)
      : reader_(reader), state_(state) {}

  StepResult Step(uint32_t address, bool allow_follow);

 private:
  PpcPrologValue Base(uint32_t a) const {
    return a ? state_->gpr[a] : Constant(0);
  }
  void SetGpr(uint32_t index, const PpcPrologValue& value) {
    if (index == 1 && value.kind != Kind::kStackRelative) {
      state_->dynamic_frame = true;
    }
    state_->gpr[index] = value;
  }
  void Store(const PpcPrologValue& base, int64_t displacement,
             const PpcPrologValue& value, uint8_t size);
  bool FollowMillicode(uint32_t target);
  void ClobberAfterOpaqueCall();

  const GuestMemoryReader& reader_;
  PpcPrologState* state_;
};

void PrologSimulator::Store(const PpcPrologValue& base, int64_t displacement,
                            const PpcPrologValue& value, uint8_t size) {
  if (base.kind != Kind::kStackRelative) {
    return;
  }
  const int64_t offset = base.value + displacement;
  if (offset < INT32_MIN || offset > INT32_MAX) {
    return;
  }
  // A later store to a slot replaces what it held.
  for (uint32_t i = 0; i < state_->slot_count; ++i) {
    PpcPrologSaveSlot& slot = state_->slots[i];
    if (slot.offset == offset) {
      slot.saved = IsEntryValue(value) ? value : Unknown();
      slot.size = size;
      return;
    }
  }
  if (!IsEntryValue(value) || state_->slot_count >= state_->slots.size()) {
    return;
  }
  PpcPrologSaveSlot& slot = state_->slots[state_->slot_count++];
  slot.saved = value;
  slot.offset = static_cast<int32_t>(offset);
  slot.size = size;
}

void PrologSimulator::ClobberAfterOpaqueCall() {
  // A call the simulation cannot follow (__chkstk probing a large frame)
  // clobbers the volatile registers except r12, which __chkstk preserves
  // for the stwux r1,r1,r12 after it.
  state_->gpr[0] = Unknown();
  for (uint32_t i = 3; i <= 11; ++i) {
    state_->gpr[i] = Unknown();
  }
}

bool PrologSimulator::FollowMillicode(uint32_t target) {
  const PpcPrologState saved = *state_;
  for (uint32_t i = 0; i < kMaxMillicodeInstructions; ++i) {
    const uint32_t address = target + i * 4;
    uint32_t word;
    if (!reader_.Read32(address, &word)) {
      break;
    }
    if (word == kBlr) {
      return true;
    }
    if (Step(address, false) != StepResult::kContinue) {
      break;
    }
  }
  *state_ = saved;
  return false;
}

PrologSimulator::StepResult PrologSimulator::Step(uint32_t address,
                                                  bool allow_follow) {
  uint32_t word;
  if (!reader_.Read32(address, &word)) {
    return StepResult::kReadError;
  }
  const uint32_t d = FieldD(word);
  const uint32_t a = FieldA(word);
  const uint32_t b = FieldB(word);
  const int64_t simm = SignExtend16(word);
  auto& gpr = state_->gpr;
  auto& fpr = state_->fpr;

  switch (Opcode(word)) {
    case 18: {  // b, bl, ba, bla
      if (!(word & 1)) {
        return StepResult::kStop;
      }
      int64_t li = static_cast<int64_t>(word & 0x03FFFFFC);
      if (li & 0x02000000) {
        li -= 0x04000000;
      }
      const uint32_t target = (word & 2) ? static_cast<uint32_t>(li)
                                         : static_cast<uint32_t>(address + li);
      if (!allow_follow || !FollowMillicode(target)) {
        ClobberAfterOpaqueCall();
      }
      state_->lr = Unknown();
      return StepResult::kContinue;
    }
    case 16:  // bc
    case 17:  // sc
      return StepResult::kStop;
    case 19: {
      const uint32_t xo = ExtendedOpcode(word);
      if (xo == 16 || xo == 528) {  // bclr, bcctr
        return StepResult::kStop;
      }
      return StepResult::kContinue;  // CR logical, isync
    }
    case 2:   // tdi
    case 3:   // twi
    case 10:  // cmpli
    case 11:  // cmpi
      return StepResult::kContinue;
    case 7:   // mulli
    case 8:   // subfic
    case 12:  // addic
    case 13:  // addic.
      SetGpr(d, Unknown());
      return StepResult::kContinue;
    case 14:  // addi (li)
      SetGpr(d, Add(Base(a), Constant(simm)));
      return StepResult::kContinue;
    case 15:  // addis (lis)
      SetGpr(d, Add(Base(a), Constant(simm * 65536)));
      return StepResult::kContinue;
    case 24:    // ori
    case 25: {  // oris
      const PpcPrologValue source = gpr[d];
      int64_t immediate = static_cast<int64_t>(word & 0xFFFF);
      if (Opcode(word) == 25) {
        immediate *= 65536;
      }
      if (source.kind == Kind::kConstant) {
        SetGpr(a, Constant(source.value | immediate));
      } else {
        SetGpr(a, immediate ? Unknown() : source);
      }
      return StepResult::kContinue;
    }
    case 20:  // rlwimi
    case 21:  // rlwinm
    case 23:  // rlwnm
    case 26:  // xori
    case 27:  // xoris
    case 28:  // andi.
    case 29:  // andis.
    case 30:  // rld*
      SetGpr(a, Unknown());
      return StepResult::kContinue;
    case 32:  // lwz
    case 34:  // lbz
    case 40:  // lhz
    case 42:  // lha
      SetGpr(d, Unknown());
      return StepResult::kContinue;
    case 33:  // lwzu
    case 35:  // lbzu
    case 41:  // lhzu
    case 43:  // lhau
      SetGpr(d, Unknown());
      SetGpr(a, Unknown());
      return StepResult::kContinue;
    case 46:  // lmw
      for (uint32_t i = d; i < 32; ++i) {
        SetGpr(i, Unknown());
      }
      return StepResult::kContinue;
    case 58:  // ld, ldu, lwa
      SetGpr(d, Unknown());
      if ((word & 3) == 1) {
        SetGpr(a, Unknown());
      }
      return StepResult::kContinue;
    case 48:  // lfs
    case 50:  // lfd
      fpr[d] = Unknown();
      return StepResult::kContinue;
    case 49:  // lfsu
    case 51:  // lfdu
      fpr[d] = Unknown();
      SetGpr(a, Unknown());
      return StepResult::kContinue;
    case 36:  // stw
      Store(Base(a), simm, gpr[d], 4);
      return StepResult::kContinue;
    case 37: {  // stwu
      const PpcPrologValue base = Base(a);
      Store(base, simm, gpr[d], 4);
      SetGpr(a, Add(base, Constant(simm)));
      return StepResult::kContinue;
    }
    case 38:  // stb
    case 44:  // sth
    case 52:  // stfs
      return StepResult::kContinue;
    case 39:  // stbu
    case 45:  // sthu
    case 53:  // stfsu
      SetGpr(a, Add(Base(a), Constant(simm)));
      return StepResult::kContinue;
    case 47:  // stmw
      for (uint32_t i = d; i < 32; ++i) {
        Store(Base(a), simm + 4 * static_cast<int64_t>(i - d), gpr[i], 4);
      }
      return StepResult::kContinue;
    case 54:  // stfd
      Store(Base(a), simm, fpr[d], 8);
      return StepResult::kContinue;
    case 55: {  // stfdu
      const PpcPrologValue base = Base(a);
      Store(base, simm, fpr[d], 8);
      SetGpr(a, Add(base, Constant(simm)));
      return StepResult::kContinue;
    }
    case 62: {  // std, stdu
      const int64_t ds = SignExtend16(word & 0xFFFC);
      const uint32_t form = word & 3;
      if (form <= 1) {
        const PpcPrologValue base = Base(a);
        Store(base, ds, gpr[d], 8);
        if (form == 1) {
          SetGpr(a, Add(base, Constant(ds)));
        }
      }
      return StepResult::kContinue;
    }
    case 4:  // VMX and VMX128: vector saves are not restored.
      return StepResult::kContinue;
    case 59:
    case 63:
      fpr[d] = Unknown();
      return StepResult::kContinue;
    case 31: {
      const uint32_t xo = ExtendedOpcode(word);
      bool float_update = false;
      if (xo == 339) {  // mfspr
        SetGpr(d, SprNumber(word) == kSprLr ? state_->lr : Unknown());
      } else if (xo == 467) {  // mtspr
        if (SprNumber(word) == kSprLr) {
          state_->lr = gpr[d];
        }
      } else if (xo == 19) {  // mfcr, mfocrf
        SetGpr(d, state_->cr);
      } else if (xo == 444) {  // or (mr)
        SetGpr(a, d == b ? gpr[d] : Unknown());
      } else if (xo == 266) {  // add
        SetGpr(d, Add(gpr[a], gpr[b]));
      } else if (IsStoreIndexed(xo)) {
        const PpcPrologValue base = Add(Base(a), gpr[b]);
        if (xo == 151 || xo == 183) {  // stwx, stwux
          Store(base, 0, gpr[d], 4);
        } else if (xo == 149 || xo == 181) {  // stdx, stdux
          Store(base, 0, gpr[d], 8);
        } else if (xo == 727 || xo == 759) {  // stfdx, stfdux
          Store(base, 0, fpr[d], 8);
        }
        if (IsStoreIndexedWithUpdate(xo)) {
          SetGpr(a, base);
        }
      } else if (IsLogicalIndexed(xo)) {
        SetGpr(a, Unknown());
      } else if (IsLoadIndexedWithUpdate(xo)) {
        SetGpr(d, Unknown());
        SetGpr(a, Unknown());
      } else if (IsFloatLoadIndexed(xo, &float_update)) {
        fpr[d] = Unknown();
        if (float_update) {
          SetGpr(a, Unknown());
        }
      } else if (!HasNoTrackedGprResult(xo)) {
        SetGpr(d, Unknown());
      }
      return StepResult::kContinue;
    }
    default:
      state_->saw_unknown_instruction = true;
      SetGpr(d, Unknown());
      return StepResult::kContinue;
  }
}

void InitializePrologState(PpcPrologState* state) {
  for (uint32_t i = 0; i < 32; ++i) {
    state->gpr[i] = MakeValue(Kind::kEntryGpr, static_cast<uint8_t>(i));
    state->fpr[i] = MakeValue(Kind::kEntryFpr, static_cast<uint8_t>(i));
  }
  state->gpr[1] = StackRelative(0);
  state->lr = MakeValue(Kind::kEntryLr);
  state->cr = MakeValue(Kind::kEntryCr);
  state->slot_count = 0;
  state->dynamic_frame = false;
  state->saw_unknown_instruction = false;
}

bool ReadLittleEndian32(const GuestMemoryReader& reader, uint32_t address,
                        uint32_t* value) {
  uint32_t big_endian;
  if (!reader.Read32(address, &big_endian)) {
    return false;
  }
  *value = xe::byte_swap(big_endian);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
bool EncodePpcContext(const PpcRegisterState& state, std::span<uint8_t> bytes) {
  if (bytes.size() < kPpcContextEncodedSize) {
    return false;
  }
  WriteBigEndian32(bytes, kPpcContextFlagsOffset, state.context_flags);
  WriteBigEndian32(bytes, kPpcContextMsrOffset, state.msr);
  WriteBigEndian32(bytes, kPpcContextIarOffset, state.iar);
  WriteBigEndian32(bytes, kPpcContextLrOffset, state.lr);
  WriteBigEndian64(bytes, kPpcContextCtrOffset, state.ctr);
  for (size_t i = 0; i < state.gpr.size(); ++i) {
    WriteBigEndian64(bytes, kPpcContextGprOffset + i * 8, state.gpr[i]);
  }
  WriteBigEndian32(bytes, kPpcContextCrOffset, state.cr);
  WriteBigEndian32(bytes, kPpcContextXerOffset, state.xer);
  WriteBigEndian32(bytes, kPpcContextFpscrOffset, 0);
  WriteBigEndian32(bytes, kPpcContextFpscrValueOffset, state.fpscr);
  for (size_t i = 0; i < state.fpr.size(); ++i) {
    WriteBigEndian64(bytes, kPpcContextFprOffset + i * 8, state.fpr[i]);
  }
  return true;
}

bool DecodePpcContext(std::span<const uint8_t> bytes, PpcRegisterState* state) {
  if (bytes.size() < kPpcContextEncodedSize) {
    return false;
  }
  state->context_flags = ReadBigEndian32(bytes, kPpcContextFlagsOffset);
  state->msr = ReadBigEndian32(bytes, kPpcContextMsrOffset);
  state->iar = ReadBigEndian32(bytes, kPpcContextIarOffset);
  state->lr = ReadBigEndian32(bytes, kPpcContextLrOffset);
  state->ctr = ReadBigEndian64(bytes, kPpcContextCtrOffset);
  for (size_t i = 0; i < state->gpr.size(); ++i) {
    state->gpr[i] = ReadBigEndian64(bytes, kPpcContextGprOffset + i * 8);
  }
  state->cr = ReadBigEndian32(bytes, kPpcContextCrOffset);
  state->xer = ReadBigEndian32(bytes, kPpcContextXerOffset);
  state->fpscr = ReadBigEndian32(bytes, kPpcContextFpscrValueOffset);
  for (size_t i = 0; i < state->fpr.size(); ++i) {
    state->fpr[i] = ReadBigEndian64(bytes, kPpcContextFprOffset + i * 8);
  }
  return true;
}

// ---------------------------------------------------------------------------
std::optional<PpcGuestRange> FindPeExceptionDirectory(
    const GuestMemoryReader& reader, uint32_t image_base) {
  constexpr uint32_t kDosSignature = 0x4D5A0000;  // "MZ", big-endian word
  constexpr uint32_t kPeSignature = 0x50450000;   // "PE\0\0"
  constexpr uint32_t kMaxHeaderOffset = 0x10000;
  constexpr uint32_t kPe32Magic = 0x10B;
  constexpr uint32_t kExceptionDirectoryIndex = 3;
  // IMAGE_OPTIONAL_HEADER32: NumberOfRvaAndSizes at 92, DataDirectory at 96.
  constexpr uint32_t kNumberOfRvaAndSizesOffset = 92;
  constexpr uint32_t kDataDirectoryOffset = 96;

  uint32_t word;
  if (!reader.Read32(image_base, &word) ||
      (word & 0xFFFF0000) != kDosSignature) {
    return std::nullopt;
  }
  uint32_t pe_offset;
  if (!ReadLittleEndian32(reader, image_base + 0x3C, &pe_offset) ||
      pe_offset >= kMaxHeaderOffset) {
    return std::nullopt;
  }
  const uint32_t pe_header = image_base + pe_offset;
  if (!reader.Read32(pe_header, &word) || word != kPeSignature) {
    return std::nullopt;
  }
  const uint32_t optional_header = pe_header + 24;
  uint32_t magic;
  uint32_t directory_count;
  if (!ReadLittleEndian32(reader, optional_header, &magic) ||
      (magic & 0xFFFF) != kPe32Magic ||
      !ReadLittleEndian32(reader, optional_header + kNumberOfRvaAndSizesOffset,
                          &directory_count) ||
      directory_count <= kExceptionDirectoryIndex) {
    return std::nullopt;
  }
  const uint32_t entry =
      optional_header + kDataDirectoryOffset + kExceptionDirectoryIndex * 8;
  PpcGuestRange range;
  uint32_t rva;
  if (!ReadLittleEndian32(reader, entry, &rva) ||
      !ReadLittleEndian32(reader, entry + 4, &range.size) || !rva ||
      range.size < 8) {
    return std::nullopt;
  }
  range.address = image_base + rva;
  return range;
}

std::optional<PpcRuntimeFunction> LookupPpcRuntimeFunction(
    const GuestMemoryReader& reader, PpcGuestRange table, uint32_t pc) {
  uint32_t count = table.size / 8;
  // The linker pads the table; its tail entries are zero.
  while (count) {
    uint32_t begin;
    if (!reader.Read32(table.address + (count - 1) * 8, &begin)) {
      return std::nullopt;
    }
    if (begin) {
      break;
    }
    --count;
  }
  // The last entry whose BeginAddress is at or below pc.
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    uint32_t begin;
    if (!reader.Read32(table.address + middle * 8, &begin)) {
      return std::nullopt;
    }
    if (begin <= pc) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (!low) {
    return std::nullopt;
  }
  PpcRuntimeFunction function;
  function.entry_address = table.address + (low - 1) * 8;
  if (!reader.Read32(function.entry_address, &function.begin_address) ||
      !reader.Read32(function.entry_address + 4, &function.packed)) {
    return std::nullopt;
  }
  if (pc >= function.end_address()) {
    return std::nullopt;
  }
  return function;
}

// ---------------------------------------------------------------------------
const PpcPrologSaveSlot* PpcPrologState::FindSlot(
    const PpcPrologValue& saved) const {
  for (uint32_t i = 0; i < slot_count; ++i) {
    if (slots[i].saved == saved) {
      return &slots[i];
    }
  }
  return nullptr;
}

bool SimulatePpcProlog(const GuestMemoryReader& reader, uint32_t begin_address,
                       uint32_t stop_address, PpcPrologState* state) {
  InitializePrologState(state);
  PrologSimulator simulator(reader, state);
  for (uint32_t address = begin_address; address < stop_address; address += 4) {
    const auto result = simulator.Step(address, true);
    if (result == PrologSimulator::StepResult::kReadError) {
      return false;
    }
    if (result == PrologSimulator::StepResult::kStop) {
      break;
    }
  }
  return true;
}

bool PpcVirtualUnwind(const GuestMemoryReader& reader,
                      const PpcRuntimeFunction& function, uint32_t control_pc,
                      const PpcRegisterState& state, PpcUnwindResult* result) {
  if (control_pc < function.begin_address ||
      control_pc >= function.end_address()) {
    return false;
  }
  const uint32_t prolog_end = function.prolog_end();
  PpcPrologState prolog;
  if (!SimulatePpcProlog(reader, function.begin_address,
                         std::min(control_pc, prolog_end), &prolog)) {
    return false;
  }
  const bool in_body = control_pc >= prolog_end;

  // A nonvolatile register set relative to r1 in the prolog (addi r31,r1,-N
  // before stwu, or mr r31,r1 after it) is the frame pointer; it survives
  // alloca in the body, which moves r1.
  int frame_pointer = -1;
  for (int i = 31; i >= 14; --i) {
    if (prolog.gpr[i].kind == Kind::kStackRelative) {
      frame_pointer = i;
      break;
    }
  }
  auto from_frame_pointer = [&]() {
    return static_cast<uint32_t>(
        static_cast<int64_t>(static_cast<uint32_t>(state.gpr[frame_pointer])) -
        prolog.gpr[frame_pointer].value);
  };

  const uint32_t sp = static_cast<uint32_t>(state.gpr[1]);
  uint32_t entry_sp = 0;
  const PpcPrologValue& r1 = prolog.gpr[1];
  if (!prolog.dynamic_frame && r1.kind == Kind::kStackRelative) {
    const uint32_t from_sp =
        static_cast<uint32_t>(static_cast<int64_t>(sp) - r1.value);
    if (!r1.value) {
      entry_sp = sp;
    } else {
      // stwu stored the entry r1 at the new r1. A mismatch means alloca
      // moved r1 since; the frame pointer then still locates the frame.
      uint32_t back_chain;
      if (reader.Read32(sp, &back_chain) && back_chain == from_sp) {
        entry_sp = from_sp;
      } else if (frame_pointer >= 0) {
        entry_sp = from_frame_pointer();
      } else {
        entry_sp = from_sp;
      }
    }
  } else if (frame_pointer >= 0) {
    entry_sp = from_frame_pointer();
  } else {
    // stwux r1,r1,rN allocated a frame of run-time size: follow the back
    // chain it stored.
    if (!reader.Read32(sp, &entry_sp)) {
      return false;
    }
  }
  if (entry_sp < sp) {
    return false;
  }

  // Where an entry value is now: a save slot, a register still holding it
  // (only a nonvolatile one once the body has run), or still in place.
  bool read_ok = true;
  auto locate = [&](const PpcPrologValue& wanted, uint64_t current) {
    if (const PpcPrologSaveSlot* slot = prolog.FindSlot(wanted)) {
      const uint32_t address =
          static_cast<uint32_t>(static_cast<int64_t>(entry_sp) + slot->offset);
      if (slot->size == 8) {
        uint64_t value = 0;
        read_ok &= reader.Read64(address, &value);
        return value;
      }
      uint32_t value = 0;
      read_ok &= reader.Read32(address, &value);
      return static_cast<uint64_t>(value);
    }
    if (wanted.kind == Kind::kEntryFpr) {
      for (uint32_t i = in_body ? 14 : 0; i < 32; ++i) {
        if (prolog.fpr[i] == wanted) {
          return state.fpr[i];
        }
      }
      return current;
    }
    for (uint32_t i = in_body ? 14 : 0; i < 32; ++i) {
      if (prolog.gpr[i] == wanted) {
        return state.gpr[i];
      }
    }
    return current;
  };

  PpcRegisterState& caller = result->caller;
  caller = state;
  caller.lr =
      static_cast<uint32_t>(locate(MakeValue(Kind::kEntryLr), state.lr));
  caller.cr =
      static_cast<uint32_t>(locate(MakeValue(Kind::kEntryCr), state.cr));
  for (uint32_t i = 14; i < 32; ++i) {
    caller.gpr[i] = locate(MakeValue(Kind::kEntryGpr, static_cast<uint8_t>(i)),
                           state.gpr[i]);
    caller.fpr[i] = locate(MakeValue(Kind::kEntryFpr, static_cast<uint8_t>(i)),
                           state.fpr[i]);
  }
  if (!read_ok) {
    return false;
  }
  caller.gpr[1] = entry_sp;
  caller.iar = caller.lr;
  result->establisher_frame = entry_sp;
  result->in_function = in_body;
  return true;
}

// ---------------------------------------------------------------------------
std::optional<uint32_t> ReadCScopeCount(const GuestMemoryReader& reader,
                                        uint32_t scope_table) {
  uint32_t count;
  if (!scope_table || !reader.Read32(scope_table, &count) ||
      count > kMaxCScopeRecords) {
    return std::nullopt;
  }
  return count;
}

bool ReadCScopeRecord(const GuestMemoryReader& reader, uint32_t scope_table,
                      uint32_t index, CScopeRecord* record) {
  const uint32_t address = scope_table + 4 + index * 16;
  return reader.Read32(address, &record->begin_address) &&
         reader.Read32(address + 4, &record->end_address) &&
         reader.Read32(address + 8, &record->handler_address) &&
         reader.Read32(address + 12, &record->jump_target);
}

CScopeDispatchDecision DispatchCScopeTable(const GuestMemoryReader& reader,
                                           uint32_t scope_table,
                                           uint32_t control_pc,
                                           CScopeFilterCallback filter,
                                           void* opaque) {
  CScopeDispatchDecision decision;
  const std::optional<uint32_t> count = ReadCScopeCount(reader, scope_table);
  if (!count) {
    decision.action = CScopeDispatchAction::kBadTable;
    return decision;
  }
  for (uint32_t i = 0; i < *count; ++i) {
    CScopeRecord scope;
    if (!ReadCScopeRecord(reader, scope_table, i, &scope)) {
      decision.action = CScopeDispatchAction::kBadTable;
      return decision;
    }
    if (control_pc < scope.begin_address || control_pc >= scope.end_address ||
        !scope.jump_target) {
      continue;
    }
    const int32_t value = scope.handler_address == kCScopeExecuteHandlerFilter
                              ? 1
                              : filter(opaque, scope);
    if (value < 0) {
      decision.action = CScopeDispatchAction::kContinueExecution;
      decision.scope = scope;
      return decision;
    }
    if (value > 0) {
      decision.action = CScopeDispatchAction::kExecuteHandler;
      decision.scope = scope;
      return decision;
    }
  }
  decision.action = CScopeDispatchAction::kContinueSearch;
  return decision;
}

bool UnwindCScopeTable(const GuestMemoryReader& reader, uint32_t scope_table,
                       uint32_t control_pc, uint32_t target_pc,
                       CScopeTerminationCallback terminate, void* opaque) {
  const std::optional<uint32_t> count = ReadCScopeCount(reader, scope_table);
  if (!count) {
    return false;
  }
  for (uint32_t i = 0; i < *count; ++i) {
    CScopeRecord scope;
    if (!ReadCScopeRecord(reader, scope_table, i, &scope)) {
      return false;
    }
    if (control_pc < scope.begin_address || control_pc >= scope.end_address) {
      continue;
    }
    // An unwind into this same scope (an uplevel goto out of an inner scope
    // or a longjmp back into it, or a leave just past its end) stops here.
    if (target_pc >= scope.begin_address && target_pc <= scope.end_address) {
      break;
    }
    if (scope.jump_target) {
      // The __except block this unwind is heading for.
      if (target_pc == scope.jump_target) {
        break;
      }
    } else {
      terminate(opaque, scope);
    }
  }
  return true;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe

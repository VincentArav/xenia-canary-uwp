/*
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_emit-private.h"

#include "xenia/base/assert.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_hir_builder.h"

#include <stddef.h>

namespace xe {
namespace cpu {
namespace ppc {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::RoundMode;
using xe::cpu::hir::Value;

// Good source of information:
// https://github.com/mamedev/historic-mame/blob/master/src/emu/cpu/powerpc/ppc_ops.c
// The correctness of that code is not reflected here yet -_-

// Enable rounding numbers to single precision as required.
// This adds a bunch of work per operation and I'm not sure it's required.
#define ROUND_TO_SINGLE

// Floating-point arithmetic (A-8)

int InstrEmit_faddx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) + (frB)
  Value* v = f.Add(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_faddsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) + (frB)
  Value* v = f.Add(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fdivx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- frA / frB
#if XE_PLATFORM_LINUX
  // TODO(has207): verify if this is needed on Windows
  // On Linux, fdiv needs to use A format fields instead of X format
  Value* v = f.Div(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
#else
  Value* v = f.Div(f.LoadFPR(i.X.RA), f.LoadFPR(i.X.RB));
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
#endif
  return 0;
}

int InstrEmit_fdivsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- frA / frB
#if XE_PLATFORM_LINUX
  // TODO(has207): verify if this is needed on Windows
  // On Linux, fdivs needs to use A format fields instead of X format
  Value* v = f.Div(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
#else
  Value* v = f.Div(f.LoadFPR(i.X.RA), f.LoadFPR(i.X.RB));
  v = f.ToSingle(v);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
#endif
  return 0;
}

int InstrEmit_fmulx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) x (frC)
  Value* v = f.Mul(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmulsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) x (frC)
  Value* v = f.Mul(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fresx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- 1.0 / (frB)

  // this actually does seem to require single precision, oddly
  // more research is needed
  Value* v = f.Recip(f.Convert(f.LoadFPR(i.A.FRB), FLOAT32_TYPE));
  v = f.Convert(v, FLOAT64_TYPE);  // f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_frsqrtex(PPCHIRBuilder& f, const InstrData& i) {
  // Double precision:
  // frD <- 1/sqrt(frB)
  Value* v = f.RSqrt(f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsubx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) - (frB)
  Value* v = f.Sub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsubsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) - (frB)
  Value* v = f.Sub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fselx(PPCHIRBuilder& f, const InstrData& i) {
  // if (frA) >= 0.0
  // then frD <- (frC)
  // else frD <- (frB)
  Value* ge = f.CompareSGE(f.LoadFPR(i.A.FRA), f.LoadZeroFloat64());
  Value* v = f.Select(ge, f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}
static int InstrEmit_fsqrt(PPCHIRBuilder& f, const InstrData& i, bool single) {
  // frD <- sqrt(frB)
  Value* v = f.Sqrt(f.LoadFPR(i.A.FRB));
  if (single) {
    v = f.ToSingle(v);
  }
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}
int InstrEmit_fsqrtx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fsqrt(f, i, false);
}

int InstrEmit_fsqrtsx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fsqrt(f, i, true);
}

// Floating-point multiply-add (A-9)

static int InstrEmit_fmadd(PPCHIRBuilder& f, const InstrData& i, bool single) {
  // frD <- (frA x frC) + frB
  Value* v =
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  if (single) {
    v = f.ToSingle(v);
  }
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmaddx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fmadd(f, i, false);
}

int InstrEmit_fmaddsx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fmadd(f, i, true);
}

static int InstrEmit_fmsub(PPCHIRBuilder& f, const InstrData& i, bool single) {
  // frD <- (frA x frC) - frB
  Value* v =
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  if (single) {
    v = f.ToSingle(v);
  }
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}
int InstrEmit_fmsubx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fmsub(f, i, false);
}

int InstrEmit_fmsubsx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fmsub(f, i, true);
}

int InstrEmit_fnmaddx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] + frB)
  Value* v = f.Neg(
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmaddsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] + frB)
  Value* v = f.Neg(
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmsubx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] - frB)
  Value* v = f.Neg(
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmsubsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] - frB)
  Value* v = f.Neg(
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  v = f.ToSingle(v);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

// Floating-point rounding and conversion (A-10)

int InstrEmit_fcfidx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- signed_int64_to_double( frB )
  Value* v = f.Convert(f.Cast(f.LoadFPR(i.X.RB), INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

// In checked code an invalid conversion sets VXCVI and VX, FX when an exception
// bit changes from 0 to 1, and VXSNAN for a signaling NaN, and clears FR and
// FI, as the ISA specifies. Other code keeps the unflagged conversions.
static void EmitInvalidConversion(PPCHIRBuilder& f, Value* snan_bit) {
  constexpr uint32_t kFx = 0x80000000;
  constexpr uint32_t kVx = 0x20000000;
  constexpr uint32_t kFrFi = 0x00060000;
  constexpr uint32_t kVxcvi = 0x00000100;
  Value* fpscr = f.LoadFPSCR();
  Value* raised = f.LoadConstantUint32(kVxcvi);
  if (snan_bit) {
    raised = f.Or(raised, f.Shl(snan_bit, int8_t(24)));  // VXSNAN
  }
  Value* newly_raised = f.And(f.Not(fpscr), raised);
  Value* fx = f.Shl(
      f.ZeroExtend(f.CompareNE(newly_raised, f.LoadZeroInt32()), INT32_TYPE),
      int8_t(31));
  static_assert(kFx == 1u << 31);
  fpscr = f.And(fpscr, f.LoadConstantUint32(~kFrFi));
  fpscr = f.Or(fpscr, f.Or(raised, f.Or(f.LoadConstantUint32(kVx), fx)));
  f.StoreFPSCR(fpscr);
}

// Whether the rounded value lies outside [min, max_exclusive).
static Value* ConversionOutOfRange(PPCHIRBuilder& f, Value* value,
                                   RoundMode round_mode, double min,
                                   double max_exclusive) {
  Value* rounded = f.Round(value, round_mode);
  return f.Or(f.CompareSGE(rounded, f.LoadConstantFloat64(max_exclusive)),
              f.CompareSLT(rounded, f.LoadConstantFloat64(min)));
}

// 1 for a signaling NaN (its most significant fraction bit is clear).
static Value* SignalingNanBit(PPCHIRBuilder& f, Value* nan) {
  Value* bits = f.Cast(nan, INT64_TYPE);
  return f.Truncate(
      f.And(f.Shr(f.Not(bits), int8_t(51)), f.LoadConstantUint64(1)),
      INT32_TYPE);
}

int InstrEmit_fctidxx_(PPCHIRBuilder& f, const InstrData& i,
                       RoundMode round_mode) {
  auto end = f.NewLabel();
  auto isnan = f.NewLabel();
  const bool flag_invalid = f.is_checked_user_mode();
  Value* v;
  f.BranchTrue(f.IsNan(f.LoadFPR(i.X.RB)), isnan);
  Value* b = f.LoadFPR(i.X.RB);
  Value* out_of_range =
      flag_invalid
          ? ConversionOutOfRange(f, b, round_mode, -9223372036854775808.0,
                                 9223372036854775808.0)
          : nullptr;
  v = f.Convert(b, INT64_TYPE, round_mode);
  v = f.Cast(v, FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  if (out_of_range) {
    f.BranchFalse(out_of_range, end);
    EmitInvalidConversion(f, nullptr);
  }
  f.Branch(end);
  f.MarkLabel(isnan);
  // Read the input before the result may overwrite it (RT == RB).
  Value* snan_bit =
      flag_invalid ? SignalingNanBit(f, f.LoadFPR(i.X.RB)) : nullptr;
  v = f.Cast(f.LoadConstantUint64(0x8000000000000000u), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  if (flag_invalid) {
    // After UpdateFPSCR, which clears FEX and VX.
    EmitInvalidConversion(f, snan_bit);
  }
  f.MarkLabel(end);
  if (flag_invalid && i.X.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

int InstrEmit_fctidx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- double_to_signed_int64( frB )
  return InstrEmit_fctidxx_(f, i, ROUND_DYNAMIC);
}

int InstrEmit_fctidzx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fctidxx_(f, i, ROUND_TO_ZERO);
}

int InstrEmit_fctiwxx_(PPCHIRBuilder& f, const InstrData& i,
                       RoundMode round_mode) {
  auto end = f.NewLabel();
  auto isnan = f.NewLabel();
  const bool flag_invalid = f.is_checked_user_mode();
  Value* v;
  f.BranchTrue(f.IsNan(f.LoadFPR(i.X.RB)), isnan);
  Value* b = f.LoadFPR(i.X.RB);
  Value* out_of_range =
      flag_invalid
          ? ConversionOutOfRange(f, b, round_mode, -2147483648.0, 2147483648.0)
          : nullptr;
  v = f.Convert(b, INT32_TYPE, round_mode);
  v = f.Cast(f.SignExtend(v, INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  if (out_of_range) {
    f.BranchFalse(out_of_range, end);
    EmitInvalidConversion(f, nullptr);
  }
  f.Branch(end);
  f.MarkLabel(isnan);
  // Read the input before the result may overwrite it (RT == RB).
  Value* snan_bit =
      flag_invalid ? SignalingNanBit(f, f.LoadFPR(i.X.RB)) : nullptr;
  v = f.Cast(f.LoadConstantUint32(0x80000000u), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  if (flag_invalid) {
    // After UpdateFPSCR, which clears FEX and VX.
    EmitInvalidConversion(f, snan_bit);
  }
  f.MarkLabel(end);
  if (flag_invalid && i.X.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

int InstrEmit_fctiwx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- double_to_signed_int32( frB )
  return InstrEmit_fctiwxx_(f, i, ROUND_DYNAMIC);
}

int InstrEmit_fctiwzx(PPCHIRBuilder& f, const InstrData& i) {
  // TODO(benvanik): assuming round to zero is always set, is that ok?
  return InstrEmit_fctiwxx_(f, i, ROUND_TO_ZERO);
}

int InstrEmit_frspx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- Round_single(frB)
  Value* v = f.Convert(f.LoadFPR(i.X.RB), FLOAT32_TYPE, ROUND_DYNAMIC);
  v = f.Convert(v, FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

// Floating-point compare (A-11)

int InstrEmit_fcmpx_(PPCHIRBuilder& f, const InstrData& i, bool ordered) {
  // if (FRA) is a NaN or (FRB) is a NaN then
  //   c <- 0b0001
  // else if (FRA) < (FRB) then
  //   c <- 0b1000
  // else if (FRA) > (FRB) then
  //   c <- 0b0100
  // else {
  //   c <- 0b0010
  // }
  // FPCC <- c
  // CR[4*BF:4*BF+3] <- c
  // if (FRA) is an SNaN or (FRB) is an SNaN then
  //   VXSNAN <- 1

  // TODO(benvanik): update FPCC for mffsx/etc
  // TODO(benvanik): update VXSNAN
  const uint32_t crf = i.X.RT >> 2;
  Value* ra = f.LoadFPR(i.X.RA);
  Value* rb = f.LoadFPR(i.X.RB);

  Value* nan = f.Or(f.IsNan(ra), f.IsNan(rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 3, nan);
  Value* not_nan = f.Xor(nan, f.LoadConstantInt8(0x01));

  Value* lt = f.And(not_nan, f.CompareSLT(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 0, lt);
  Value* gt = f.And(not_nan, f.CompareSGT(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 1, gt);
  Value* eq = f.And(not_nan, f.CompareEQ(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 2, eq);
  return 0;
}
int InstrEmit_fcmpo(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fcmpx_(f, i, true);
}
int InstrEmit_fcmpu(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fcmpx_(f, i, false);
}

// Floating-point status and control register (A

int InstrEmit_mcrfs(PPCHIRBuilder& f, const InstrData& i) {
  // mcrfs CRFD, CRFS
  // CR[4*CRFD:4*CRFD+3] <- FPSCR[4*CRFS:4*CRFS+3]
  // Only the field's exception bits are cleared, and FEX and VX recomputed;
  // the control bits (the enables, NI and RN) are kept.
  constexpr uint32_t kExceptionBits = 0x9FF80700;
  constexpr uint32_t kInvalidBits = 0x01F80700;  // VXSNAN-VXVC, VXSOFT-VXCVI
  constexpr uint32_t kFex = 0x40000000;
  constexpr uint32_t kVx = 0x20000000;

  uint32_t crfd = i.X.RT >> 2;  // Destination CR field
  uint32_t crfs = i.X.RA >> 2;  // Source FPSCR field

  Value* fpscr = f.LoadFPSCR();

  // Extract 4-bit field from FPSCR
  // FPSCR fields are numbered from left to right (0-7), with field 0 being bits
  // 0-3
  uint32_t shift = 4 * (7 - crfs);
  Value* fpscr_field = f.And(f.Shr(fpscr, shift), f.LoadConstantUint32(0xF));

  // Store to CR field (need to shift to proper position in 64-bit CR)
  f.StoreCR(crfd, f.Shl(f.ZeroExtend(fpscr_field, INT64_TYPE), 4 * (7 - crfd)));

  const uint32_t clear = (0xFu << shift) & kExceptionBits;
  if (clear) {
    fpscr = f.And(fpscr, f.LoadConstantUint32(~(clear | kFex | kVx)));
    Value* vx =
        f.Shl(f.ZeroExtend(
                  f.CompareNE(f.And(fpscr, f.LoadConstantUint32(kInvalidBits)),
                              f.LoadZeroInt32()),
                  INT32_TYPE),
              int8_t(29));
    fpscr = f.Or(fpscr, vx);
    // VX, OX, UX, ZX and XX (bits 2-6) shifted right 22 line up with their
    // enables VE, OE, UE, ZE and XE (bits 24-28).
    Value* fex = f.Shl(
        f.ZeroExtend(f.CompareNE(f.And(f.And(f.Shr(fpscr, int8_t(22)), fpscr),
                                       f.LoadConstantUint32(0xF8)),
                                 f.LoadZeroInt32()),
                     INT32_TYPE),
        int8_t(30));
    f.StoreFPSCR(f.Or(fpscr, fex));
  }

  return 0;
}

int InstrEmit_mffsx(PPCHIRBuilder& f, const InstrData& i) {
  Value* v = f.Cast(f.ZeroExtend(f.LoadFPSCR(), INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  if (i.X.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

// FPSCR[crbD] <- 0 or 1. Bit 0 is the most significant. FEX and VX (bits 1
// and 2) are summaries that these instructions cannot alter.
static int EmitMoveToFpscrBit(PPCHIRBuilder& f, const InstrData& i, bool set) {
  const uint32_t bit = i.X.RT;
  if (bit != 1 && bit != 2) {
    const uint32_t mask = 1u << (31 - bit);
    Value* fpscr = f.LoadFPSCR();
    fpscr = set ? f.Or(fpscr, f.LoadConstantUint32(mask))
                : f.And(fpscr, f.LoadConstantUint32(~mask));
    f.StoreFPSCR(fpscr);
    // Update the system rounding mode (RN and NI).
    if (mask & 0x7) {
      f.SetRoundingMode(f.And(fpscr, f.LoadConstantUint32(7)));
    }
  }
  if (i.X.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

int InstrEmit_mtfsb0x(PPCHIRBuilder& f, const InstrData& i) {
  return EmitMoveToFpscrBit(f, i, false);
}

int InstrEmit_mtfsb1x(PPCHIRBuilder& f, const InstrData& i) {
  return EmitMoveToFpscrBit(f, i, true);
}

int InstrEmit_mtfsfx(PPCHIRBuilder& f, const InstrData& i) {
  if (i.XFL.L) {
    // Move/shift.
    f.StoreFPSCR(
        f.Truncate(f.Cast(f.LoadFPR(i.XFL.RB), INT64_TYPE), INT32_TYPE));
    return 1;
  } else {
    assert_zero(i.XFL.W);
    // Store under control of mask.
    // Expand the mask from 8 bits -> 32 bits.
    uint32_t mask = 0;
    for (int j = 0; j < 8; j++) {
      if (i.XFL.FM & (1 << (j ^ 7))) {
        mask |= 0xF << (4 * j);
      }
    }

    Value* v = f.Truncate(f.Cast(f.LoadFPR(i.XFL.RB), INT64_TYPE), INT32_TYPE);
    if (mask != 0xFFFFFFFF) {
      Value* fpscr = f.LoadFPSCR();
      v = f.And(v, f.LoadConstantInt32(mask));
      v = f.Or(v, f.And(fpscr, f.LoadConstantInt32(~mask)));
    }
    f.StoreFPSCR(v);

    // Update the system rounding mode.
    if (mask & 0x7) {
      f.SetRoundingMode(f.And(v, f.LoadConstantInt32(7)));
    }
  }
  if (i.XFL.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

int InstrEmit_mtfsfix(PPCHIRBuilder& f, const InstrData& i) {
  // FPSCR[crfD] <- IMM

  // Create a mask.
  uint32_t mask = 0xF << (0x1C - (i.X.RT & 0x1C));
  uint32_t value = i.X.RB << (0x1C - (i.X.RT & 0x1C));

  Value* fpscr = f.LoadFPSCR();
  fpscr = f.And(fpscr, f.LoadConstantInt32(~mask));
  fpscr = f.Or(fpscr, f.LoadConstantInt32(value));
  f.StoreFPSCR(fpscr);

  // Update the system rounding mode.
  if (mask & 0x7) {
    f.SetRoundingMode(f.And(fpscr, f.LoadConstantInt32(7)));
  }

  if (i.X.Rc) {
    f.CopyFPSCRToCR1();
  }

  return 0;
}

// Floating-point move (A-21)

int InstrEmit_fabsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- abs(frB)
  Value* v = f.Abs(f.LoadFPR(i.X.RB));
  f.StoreFPR(i.X.RT, v);
  /*
  The contents of frB with bit 0 cleared are placed into frD.
Note that the fabs instruction treats NaNs just like any other kind of value.
That is, the sign bit of a NaN may be altered by fabs. This instruction does not
alter the FPSCR. Other registers altered: • Condition Register (CR1 field):
Affected: FX, FEX, VX, OX (if Rc = 1)
  */
  // f.UpdateFPSCR(v, i.X.Rc);
  if (i.X.Rc) {
    // todo
  }
  return 0;
}

int InstrEmit_fmrx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frB)
  Value* v = f.LoadFPR(i.X.RB);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

int InstrEmit_fnabsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- !abs(frB)
  Value* v = f.Neg(f.Abs(f.LoadFPR(i.X.RB)));
  f.StoreFPR(i.X.RT, v);
  // f.UpdateFPSCR(v, i.X.Rc);
  if (i.X.Rc) {
    // todo
  }
  return 0;
}

int InstrEmit_fnegx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- ¬ frB[0] || frB[1-63]
  Value* v = f.Neg(f.LoadFPR(i.X.RB));
  f.StoreFPR(i.X.RT, v);
  // f.UpdateFPSCR(v, i.X.Rc);
  if (i.X.Rc) {
    // todo
  }
  return 0;
}

void RegisterEmitCategoryFPU() {
  XEREGISTERINSTR(faddx);
  XEREGISTERINSTR(faddsx);
  XEREGISTERINSTR(fdivx);
  XEREGISTERINSTR(fdivsx);
  XEREGISTERINSTR(fmulx);
  XEREGISTERINSTR(fmulsx);
  XEREGISTERINSTR(fresx);
  XEREGISTERINSTR(frsqrtex);
  XEREGISTERINSTR(fsubx);
  XEREGISTERINSTR(fsubsx);
  XEREGISTERINSTR(fselx);
  XEREGISTERINSTR(fsqrtx);
  XEREGISTERINSTR(fsqrtsx);
  XEREGISTERINSTR(fmaddx);
  XEREGISTERINSTR(fmaddsx);
  XEREGISTERINSTR(fmsubx);
  XEREGISTERINSTR(fmsubsx);
  XEREGISTERINSTR(fnmaddx);
  XEREGISTERINSTR(fnmaddsx);
  XEREGISTERINSTR(fnmsubx);
  XEREGISTERINSTR(fnmsubsx);
  XEREGISTERINSTR(fcfidx);
  XEREGISTERINSTR(fctidx);
  XEREGISTERINSTR(fctidzx);
  XEREGISTERINSTR(fctiwx);
  XEREGISTERINSTR(fctiwzx);
  XEREGISTERINSTR(frspx);
  XEREGISTERINSTR(fcmpo);
  XEREGISTERINSTR(fcmpu);
  XEREGISTERINSTR(mcrfs);
  XEREGISTERINSTR(mffsx);
  XEREGISTERINSTR(mtfsb0x);
  XEREGISTERINSTR(mtfsb1x);
  XEREGISTERINSTR(mtfsfx);
  XEREGISTERINSTR(mtfsfix);
  XEREGISTERINSTR(fabsx);
  XEREGISTERINSTR(fmrx);
  XEREGISTERINSTR(fnabsx);
  XEREGISTERINSTR(fnegx);
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

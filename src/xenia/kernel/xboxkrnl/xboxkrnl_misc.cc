/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/ldi_decompressor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void KeEnableFpuExceptions_entry(
    const ppc_context_t& ctx) {  // dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
  // theres a lot more thats supposed to happen here, the floating point state
  // has to be saved to kthread, the irql changes, the machine state register is
  // changed to enable exceptions

  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);
  kthread->fpu_exceptions_on = static_cast<uint32_t>(ctx->r[3]) != 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

void KeSaveFloatingPointState_entry(const ppc_context_t& ctx) {
  // Probably we should use: thread_fpu_related to store/restore state
  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    kthread->fpu_context[i] = ctx->f[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kSketchy);

void KeRestoreFloatingPointState_entry(const ppc_context_t& ctx) {
  const X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    ctx->f[i] = kthread->fpu_context[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kSketchy);

static qword_result_t KeQueryInterruptTime_entry(const ppc_context_t& ctx) {
  auto kstate = ctx->kernel_state;
  uint32_t ts_bundle = kstate->GetKeTimestampBundle();
  X_TIME_STAMP_BUNDLE* bundle =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle);

  return xe::load_and_swap<uint64_t>(&bundle->interrupt_time);
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryInterruptTime, kNone, kImplemented);

// LDI, the kernel's block-at-a-time LZX decoder (the Cabinet SDK's ldi.h
// interface), on top of libmspack's lzxd. The decoder state stays on the
// host, keyed by the handle the guest holds.
namespace {

// LDICreateDecompression's pvConfiguration for LZX.
struct X_LZX_DECOMPRESS {
  xe::be<uint32_t> window_size;  // 0x0
  // The Cabinet SDK keeps a CPU type here; the LZX bitstream doesn't depend on
  // it.
  xe::be<uint32_t> unknown_04;  // 0x4
};
static_assert_size(X_LZX_DECOMPRESS, 8);

struct LdiContext {
  std::unique_ptr<ldi::LdiDecompressor> decompressor;
  // System heap block standing in for the handle when the caller gave no
  // context memory; 0 when the handle is the caller's memory.
  uint32_t handle_block = 0;
};

std::mutex ldi_mutex;
std::unordered_map<uint32_t, LdiContext> ldi_contexts;

}  // namespace

// The 360's LDICreateDecompression takes memory for the context after the
// allocator pair, which the Cabinet SDK's doesn't. The handle is that memory's
// address, or a small system heap block when there is none; the allocator
// pair is never called.
dword_result_t LDICreateDecompression_entry(
    lpdword_t data_block_max_ptr, pointer_t<X_LZX_DECOMPRESS> configuration,
    dword_t alloc_routine, dword_t free_routine, dword_t context_memory,
    lpdword_t src_buffer_min_ptr, lpdword_t handle_ptr,
    const ppc_context_t& ctx) {
  if (!data_block_max_ptr || !handle_ptr) {
    return ldi::kBadParameters;
  }
  if (!configuration) {
    return ldi::kConfiguration;
  }
  const uint32_t window_size = configuration->window_size;
  if (!ldi::LdiDecompressor::IsValidWindowSize(window_size)) {
    XELOGW("LDICreateDecompression: unsupported LZX window size {:08X}",
           window_size);
    return ldi::kConfiguration;
  }
  auto decompressor =
      ldi::LdiDecompressor::Create(window_size, data_block_max_ptr.value());
  if (!decompressor) {
    return ldi::kNotEnoughMemory;
  }
  const uint32_t data_block_max = decompressor->data_block_max();

  uint32_t handle = context_memory;
  uint32_t handle_block = 0;
  if (!handle) {
    handle_block = ctx->kernel_state->memory()->SystemHeapAlloc(4);
    if (!handle_block) {
      return ldi::kNotEnoughMemory;
    }
    handle = handle_block;
  }
  {
    std::lock_guard<std::mutex> lock(ldi_mutex);
    // Context memory handed in again starts a new stream in it.
    LdiContext& context = ldi_contexts[handle];
    context.decompressor = std::move(decompressor);
    context.handle_block = handle_block;
  }

  *data_block_max_ptr = data_block_max;
  if (src_buffer_min_ptr) {
    *src_buffer_min_ptr = ldi::SourceBufferMin(data_block_max);
  }
  *handle_ptr = handle;
  return ldi::kNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

// Decompresses one block: src_size compressed bytes into the block's
// uncompressed size, which *dst_size_ptr holds on entry and receives the
// bytes produced.
dword_result_t LDIDecompress_entry(dword_t handle, lpvoid_t src,
                                   dword_t src_size, lpvoid_t dst,
                                   lpdword_t dst_size_ptr) {
  if (!dst_size_ptr) {
    return ldi::kBadParameters;
  }
  std::lock_guard<std::mutex> lock(ldi_mutex);
  auto it = ldi_contexts.find(static_cast<uint32_t>(handle));
  if (it == ldi_contexts.end()) {
    return ldi::kBadParameters;
  }
  const uint32_t requested = dst_size_ptr.value();
  uint32_t dst_size = requested;
  ldi::Result result = it->second.decompressor->Decompress(
      src.as<const uint8_t*>(), src_size, dst.as<uint8_t*>(), &dst_size);
  *dst_size_ptr = dst_size;
  if (result != ldi::kNoError) {
    XELOGW("LDIDecompress({:08X}): {:X} bytes to {:X} failed ({})",
           static_cast<uint32_t>(handle), static_cast<uint32_t>(src_size),
           requested, static_cast<int>(result));
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

dword_result_t LDIResetDecompression_entry(dword_t handle) {
  std::lock_guard<std::mutex> lock(ldi_mutex);
  auto it = ldi_contexts.find(static_cast<uint32_t>(handle));
  if (it == ldi_contexts.end()) {
    return ldi::kBadParameters;
  }
  return it->second.decompressor->Reset();
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kImplemented);

dword_result_t LDIDestroyDecompression_entry(dword_t handle,
                                             const ppc_context_t& ctx) {
  uint32_t handle_block = 0;
  {
    std::lock_guard<std::mutex> lock(ldi_mutex);
    auto it = ldi_contexts.find(static_cast<uint32_t>(handle));
    if (it == ldi_contexts.end()) {
      return ldi::kBadParameters;
    }
    handle_block = it->second.handle_block;
    ldi_contexts.erase(it);
  }
  if (handle_block) {
    ctx->kernel_state->memory()->SystemHeapFree(handle_block);
  }
  return ldi::kNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);

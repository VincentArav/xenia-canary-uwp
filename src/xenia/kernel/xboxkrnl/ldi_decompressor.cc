/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/ldi_decompressor.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "xenia/base/math.h"

// lzx.h uses off_t without including what declares it, so mspack.h first.
#include "third_party/mspack/mspack.h"

#include "third_party/mspack/lzx.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace ldi {

namespace {

// How many bytes lzxd pulls from the block's input at a time.
constexpr int kInputBufferSize = 0x1000;
// lzxd counts its output in a 32-bit off_t on Windows.
constexpr uint64_t kMaxStreamOutput = 0x7FFFFFFF;

// The block being decoded: its compressed input, or the caller's output.
struct MemoryFile {
  const uint8_t* read_data = nullptr;
  uint8_t* write_data = nullptr;
  uint32_t size = 0;
  uint32_t offset = 0;
};

int MemoryRead(mspack_file* file, void* buffer, int bytes) {
  auto memory = reinterpret_cast<MemoryFile*>(file);
  if (bytes < 0) {
    return -1;
  }
  uint32_t count =
      std::min(static_cast<uint32_t>(bytes), memory->size - memory->offset);
  if (count) {
    std::memcpy(buffer, memory->read_data + memory->offset, count);
    memory->offset += count;
  }
  return static_cast<int>(count);
}

int MemoryWrite(mspack_file* file, void* buffer, int bytes) {
  auto memory = reinterpret_cast<MemoryFile*>(file);
  if (bytes < 0) {
    return -1;
  }
  uint32_t count =
      std::min(static_cast<uint32_t>(bytes), memory->size - memory->offset);
  if (count) {
    std::memcpy(memory->write_data + memory->offset, buffer, count);
    memory->offset += count;
  }
  return static_cast<int>(count);
}

void* MemoryAlloc(mspack_system* self, size_t bytes) {
  return std::calloc(bytes, 1);
}

void MemoryFree(void* ptr) { std::free(ptr); }

void MemoryCopy(void* src, void* dest, size_t bytes) {
  std::memcpy(dest, src, bytes);
}

void MemoryMessage(mspack_file* file, const char* format, ...) {}

}  // namespace

struct LdiDecompressor::Stream {
  ~Stream() {
    if (lzx) {
      lzxd_free(lzx);
    }
  }

  mspack_system system = {};
  MemoryFile input;
  MemoryFile output;
  lzxd_stream* lzx = nullptr;
};

LdiDecompressor::LdiDecompressor(uint32_t window_size, uint32_t data_block_max)
    : window_size_(window_size), data_block_max_(data_block_max) {}

LdiDecompressor::~LdiDecompressor() = default;

bool LdiDecompressor::IsValidWindowSize(uint32_t window_size) {
  return window_size >= kMinWindowSize && window_size <= kMaxWindowSize &&
         xe::is_pow2(window_size);
}

std::unique_ptr<LdiDecompressor> LdiDecompressor::Create(
    uint32_t window_size, uint32_t data_block_max) {
  if (!IsValidWindowSize(window_size)) {
    return nullptr;
  }
  std::unique_ptr<LdiDecompressor> decompressor(
      new LdiDecompressor(window_size, ClampDataBlockMax(data_block_max)));
  if (!decompressor->OpenStream()) {
    return nullptr;
  }
  return decompressor;
}

bool LdiDecompressor::OpenStream() {
  stream_.reset();
  total_output_ = 0;
  finished_ = false;

  auto stream = std::make_unique<Stream>();
  stream->system.read = MemoryRead;
  stream->system.write = MemoryWrite;
  stream->system.message = MemoryMessage;
  stream->system.alloc = MemoryAlloc;
  stream->system.free = MemoryFree;
  stream->system.copy = MemoryCopy;

  uint32_t window_bits = 0;
  xe::bit_scan_forward(window_size_, &window_bits);
  stream->lzx =
      lzxd_init(&stream->system, reinterpret_cast<mspack_file*>(&stream->input),
                reinterpret_cast<mspack_file*>(&stream->output),
                static_cast<int>(window_bits), 0, kInputBufferSize, 0, 0);
  if (!stream->lzx) {
    return false;
  }
  stream_ = std::move(stream);
  return true;
}

Result LdiDecompressor::Decompress(const uint8_t* src, uint32_t src_size,
                                   uint8_t* dst, uint32_t* dst_size) {
  if (!dst_size) {
    return kBadParameters;
  }
  const uint32_t requested = *dst_size;
  *dst_size = 0;
  if (requested > data_block_max_) {
    return kBufferOverflow;
  }
  if ((src_size && !src) || (requested && !dst)) {
    return kBadParameters;
  }
  if (!requested) {
    return kNoError;
  }
  if (!stream_) {
    return kNotEnoughMemory;
  }
  lzxd_stream* lzx = stream_->lzx;
  if (finished_ || lzx->error != MSPACK_ERR_OK ||
      total_output_ + requested > kMaxStreamOutput) {
    return kFailed;
  }

  // A block's compressed bits begin on a 16-bit boundary of its own input.
  // Drop what the previous block left buffered: once that block's frame was
  // realigned, it is padding or bytes read past the end of its input.
  lzx->i_ptr = lzx->inbuf;
  lzx->i_end = lzx->inbuf;
  lzx->bit_buffer = 0;
  lzx->bits_left = 0;
  lzx->input_end = 0;

  stream_->input = MemoryFile();
  stream_->input.read_data = src;
  stream_->input.size = src_size;
  stream_->output = MemoryFile();
  stream_->output.write_data = dst;
  stream_->output.size = requested;

  // lzxd sizes the last frame from the stream length, so tell it where this
  // block ends. It decodes up to one frame past the output asked for, which
  // at the end of a whole frame is an empty frame that still counts; recount
  // the frames so that only those behind this block are, or a short last
  // block would be taken as already decoded.
  lzxd_set_output_length(lzx, static_cast<off_t>(total_output_ + requested));
  lzx->frame = static_cast<unsigned int>(total_output_ / kFrameSize);
  int status = lzxd_decompress(lzx, static_cast<off_t>(requested));
  const uint32_t produced = stream_->output.offset;
  stream_->input = MemoryFile();
  stream_->output = MemoryFile();
  if (status != MSPACK_ERR_OK || produced != requested) {
    return kFailed;
  }

  // A stored block of odd length is followed by a pad byte, which lzxd skips
  // when it reads the next block header. When the stored block ended with
  // this block, the pad was the last byte of this block's input, already
  // dropped above, so the next block's input must not lose its first byte.
  if (lzx->block_remaining == 0 &&
      lzx->block_type == LZX_BLOCKTYPE_UNCOMPRESSED &&
      (lzx->block_length & 1)) {
    lzx->block_type = LZX_BLOCKTYPE_INVALID;
  }

  total_output_ += requested;
  if (requested % kFrameSize) {
    finished_ = true;
  }
  *dst_size = requested;
  return kNoError;
}

Result LdiDecompressor::Reset() {
  return OpenStream() ? kNoError : kNotEnoughMemory;
}

}  // namespace ldi
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

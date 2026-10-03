/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_LDI_DECOMPRESSOR_H_
#define XENIA_KERNEL_XBOXKRNL_LDI_DECOMPRESSOR_H_

#include <cstdint>
#include <memory>

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace ldi {

// Results of the LDI ("LZX decompression interface") calls, as in the Cabinet
// SDK's ldi.h.
enum Result : int {
  kNoError = 0,
  kNotEnoughMemory = 1,
  kBadParameters = 2,
  kBufferOverflow = 3,
  kFailed = 4,
  kConfiguration = 5,
};

// An LZX frame: one LDIDecompress call produces at most this much.
constexpr uint32_t kFrameSize = 0x8000;
// How much larger than its output a compressed LZX frame can be (6 KiB), so
// the source buffer a caller needs for a block of kFrameSize is 0x9800.
constexpr uint32_t kMaxFrameGrowth = 0x1800;
constexpr uint32_t kMinWindowSize = 1u << 15;
constexpr uint32_t kMaxWindowSize = 1u << 21;

// The block size LDICreateDecompression settles on for a requested one: LZX
// produces at most a frame per call, and 0 asks for the default.
constexpr uint32_t ClampDataBlockMax(uint32_t requested) {
  return (requested == 0 || requested > kFrameSize) ? kFrameSize : requested;
}

// The source buffer size a caller must provide for blocks of data_block_max.
constexpr uint32_t SourceBufferMin(uint32_t data_block_max) {
  return data_block_max + kMaxFrameGrowth;
}

// One LDI decompression context: an LZX stream decoded a block at a time.
// Each Decompress call takes the compressed bytes of the next block, which
// begin on a 16-bit boundary of the LZX bitstream, and produces the block's
// output. The window, the repeated offsets and a block that runs on into the
// next frame carry over to the next call until Reset.
//
// Every block but the last of a stream must be a whole frame; after a shorter
// block the stream is finished and Decompress fails until Reset.
class LdiDecompressor {
 public:
  ~LdiDecompressor();
  LdiDecompressor(const LdiDecompressor&) = delete;
  LdiDecompressor& operator=(const LdiDecompressor&) = delete;

  // Returns nullptr unless window_size is a power of two from kMinWindowSize
  // to kMaxWindowSize (or on allocation failure, which IsValidWindowSize
  // tells apart). data_block_max is the largest block Decompress accepts; it
  // is clamped with ClampDataBlockMax.
  static std::unique_ptr<LdiDecompressor> Create(uint32_t window_size,
                                                 uint32_t data_block_max);
  static bool IsValidWindowSize(uint32_t window_size);

  // Decompresses the block in src[0, src_size) into dst. *dst_size holds the
  // block's uncompressed size on entry and the bytes produced on return (0 on
  // failure). dst must have room for the uncompressed size.
  Result Decompress(const uint8_t* src, uint32_t src_size, uint8_t* dst,
                    uint32_t* dst_size);

  // Starts a new LZX stream: clears the window, the repeated offsets and any
  // decoding error.
  Result Reset();

  uint32_t window_size() const { return window_size_; }
  uint32_t data_block_max() const { return data_block_max_; }
  // Total bytes produced since creation or the last Reset.
  uint64_t total_output() const { return total_output_; }

 private:
  struct Stream;

  LdiDecompressor(uint32_t window_size, uint32_t data_block_max);
  bool OpenStream();

  uint32_t window_size_;
  uint32_t data_block_max_;
  uint64_t total_output_ = 0;
  // Set once a block shorter than a frame ended the stream.
  bool finished_ = false;
  std::unique_ptr<Stream> stream_;
};

}  // namespace ldi
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_LDI_DECOMPRESSOR_H_

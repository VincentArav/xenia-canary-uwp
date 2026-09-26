/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xex_import_library_parser.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace xe::cpu {
namespace {

constexpr size_t kDescriptorSizeOffset = 0x00;
constexpr size_t kDescriptorIdOffset = 0x18;
constexpr size_t kDescriptorVersionOffset = 0x1C;
constexpr size_t kDescriptorMinVersionOffset = 0x20;
constexpr size_t kDescriptorNameIndexOffset = 0x24;
constexpr size_t kDescriptorImportCountOffset = 0x26;
constexpr size_t kDescriptorImportTableOffset = 0x28;

uint16_t ReadBe16(std::span<const uint8_t> bytes, size_t offset) {
  return uint16_t(uint16_t(bytes[offset]) << 8 | bytes[offset + 1]);
}

uint32_t ReadBe32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t(uint32_t(bytes[offset]) << 24 |
                  uint32_t(bytes[offset + 1]) << 16 |
                  uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3]);
}

XexImportLibraryParseResult MakeError(XexImportLibraryParseError error,
                                      size_t offset) {
  XexImportLibraryParseResult result;
  result.error = error;
  result.error_offset = offset;
  return result;
}

bool CheckedAddU32(uint32_t left, uint32_t right, uint32_t* result) {
  if (right > std::numeric_limits<uint32_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedAlign4(size_t value, size_t* result) {
  if (value > std::numeric_limits<size_t>::max() - 3) {
    return false;
  }
  *result = (value + 3) & ~size_t(3);
  return true;
}

}  // namespace

XexImportLibraryParseResult ParseXexImportLibraries(
    std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(uint32_t)) {
    return MakeError(XexImportLibraryParseError::kInputTooSmall, bytes.size());
  }

  const uint32_t declared_size = ReadBe32(bytes, 0);
  if (declared_size < kXexImportLibrariesHeaderSize) {
    return MakeError(XexImportLibraryParseError::kDeclaredSizeTooSmall, 0);
  }
  if (declared_size > bytes.size()) {
    return MakeError(XexImportLibraryParseError::kTruncated, bytes.size());
  }

  // From here on, all reads are restricted to the declared payload rather
  // than merely to the caller-provided backing span.
  bytes = bytes.first(declared_size);
  const uint32_t name_table_size = ReadBe32(bytes, 4);
  const uint32_t name_count = ReadBe32(bytes, 8);

  uint32_t descriptor_offset_u32;
  if (!CheckedAddU32(uint32_t(kXexImportLibrariesHeaderSize), name_table_size,
                     &descriptor_offset_u32)) {
    return MakeError(XexImportLibraryParseError::kIntegerOverflow, 4);
  }
  const size_t descriptor_offset = descriptor_offset_u32;
  if (descriptor_offset > bytes.size()) {
    return MakeError(XexImportLibraryParseError::kStringTableTruncated, 4);
  }

  const size_t name_table_begin = kXexImportLibrariesHeaderSize;
  const size_t name_table_end = descriptor_offset;
  std::vector<std::string> names;

  // Each padded name takes at least four bytes, which bounds the name count.
  const uint64_t maximum_name_count = (uint64_t(name_table_size) + 3) / 4;
  if (name_count > maximum_name_count) {
    return MakeError(XexImportLibraryParseError::kNameCountMismatch, 8);
  }
  names.reserve(name_count);

  size_t name_offset = name_table_begin;
  for (uint32_t i = 0; i < name_count; ++i) {
    if (name_offset >= name_table_end) {
      return MakeError(XexImportLibraryParseError::kNameCountMismatch,
                       name_offset);
    }

    const auto terminator =
        std::find(bytes.begin() + name_offset, bytes.begin() + name_table_end,
                  uint8_t(0));
    if (terminator == bytes.begin() + name_table_end) {
      return MakeError(XexImportLibraryParseError::kMissingStringTerminator,
                       name_offset);
    }

    const size_t terminator_offset =
        size_t(std::distance(bytes.begin(), terminator));
    names.emplace_back(
        reinterpret_cast<const char*>(bytes.data() + name_offset),
        terminator_offset - name_offset);

    size_t aligned_next_offset;
    if (!CheckedAlign4(terminator_offset + 1, &aligned_next_offset)) {
      return MakeError(XexImportLibraryParseError::kIntegerOverflow,
                       terminator_offset);
    }
    // The name table starts at 0x0C, so absolute and table-relative 4-byte
    // alignment are identical.
    if (aligned_next_offset > name_table_end) {
      return MakeError(XexImportLibraryParseError::kMissingStringPadding,
                       terminator_offset + 1);
    }
    // Padding contents are ignored.
    name_offset = aligned_next_offset;
  }

  if (name_offset != name_table_end) {
    return MakeError(XexImportLibraryParseError::kNameCountMismatch,
                     name_offset);
  }

  std::vector<ParsedXexImportLibrary> libraries;
  libraries.reserve(name_count);
  size_t offset = descriptor_offset;
  // The header counts names, not descriptors, and descriptors may share a
  // name, so parse descriptors to the end of the payload.
  while (offset < bytes.size()) {
    if (bytes.size() - offset < sizeof(uint32_t)) {
      return MakeError(XexImportLibraryParseError::kTruncated, offset);
    }

    const uint32_t descriptor_size =
        ReadBe32(bytes, offset + kDescriptorSizeOffset);
    if (descriptor_size == 0) {
      return MakeError(XexImportLibraryParseError::kZeroSizedDescriptor,
                       offset);
    }
    if (descriptor_size < kXexImportLibraryFixedSize) {
      return MakeError(XexImportLibraryParseError::kDescriptorTooSmall, offset);
    }

    uint32_t descriptor_end_u32;
    if (offset > std::numeric_limits<uint32_t>::max() ||
        !CheckedAddU32(uint32_t(offset), descriptor_size,
                       &descriptor_end_u32)) {
      return MakeError(XexImportLibraryParseError::kIntegerOverflow, offset);
    }
    const size_t descriptor_end = descriptor_end_u32;
    if (descriptor_end > bytes.size()) {
      return MakeError(XexImportLibraryParseError::kTruncated, offset);
    }

    const uint16_t import_count =
        ReadBe16(bytes, offset + kDescriptorImportCountOffset);
    uint32_t import_bytes;
    if (!CheckedAddU32(0, uint32_t(import_count) * sizeof(uint32_t),
                       &import_bytes)) {
      return MakeError(XexImportLibraryParseError::kIntegerOverflow,
                       offset + kDescriptorImportCountOffset);
    }
    uint32_t expected_descriptor_size;
    if (!CheckedAddU32(uint32_t(kXexImportLibraryFixedSize), import_bytes,
                       &expected_descriptor_size)) {
      return MakeError(XexImportLibraryParseError::kIntegerOverflow,
                       offset + kDescriptorImportCountOffset);
    }
    if (descriptor_size != expected_descriptor_size) {
      return MakeError(XexImportLibraryParseError::kDescriptorSizeMismatch,
                       offset);
    }

    // Only the low byte names the library, as the console reads it; the high
    // byte may be garbage.
    const uint16_t name_index =
        ReadBe16(bytes, offset + kDescriptorNameIndexOffset) & 0xFF;
    if (name_index >= names.size()) {
      return MakeError(XexImportLibraryParseError::kInvalidNameIndex,
                       offset + kDescriptorNameIndexOffset);
    }

    ParsedXexImportLibrary library;
    library.name = names[name_index];
    library.id = ReadBe32(bytes, offset + kDescriptorIdOffset);
    library.version = ReadBe32(bytes, offset + kDescriptorVersionOffset);
    library.min_version = ReadBe32(bytes, offset + kDescriptorMinVersionOffset);
    library.name_index = name_index;
    library.descriptor_offset = offset;
    library.import_records.reserve(import_count);
    for (uint16_t record_index = 0; record_index < import_count;
         ++record_index) {
      library.import_records.push_back(
          ReadBe32(bytes, offset + kDescriptorImportTableOffset +
                              size_t(record_index) * sizeof(uint32_t)));
    }
    libraries.push_back(std::move(library));
    offset = descriptor_end;
  }

  XexImportLibraryParseResult result;
  result.bytes_consumed = declared_size;
  result.libraries = std::move(libraries);
  return result;
}

}  // namespace xe::cpu

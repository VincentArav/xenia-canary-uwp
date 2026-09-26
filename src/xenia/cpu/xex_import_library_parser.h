/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_XEX_IMPORT_LIBRARY_PARSER_H_
#define XENIA_CPU_XEX_IMPORT_LIBRARY_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xe::cpu {

// Sizes and offsets of xex2_opt_import_libraries and xex2_import_library.
constexpr size_t kXexImportLibrariesHeaderSize = 0x0C;
constexpr size_t kXexImportLibraryFixedSize = 0x28;

enum class XexImportLibraryParseError {
  kNone,
  kInputTooSmall,
  kDeclaredSizeTooSmall,
  kTruncated,
  kIntegerOverflow,
  kStringTableTruncated,
  kMissingStringTerminator,
  kMissingStringPadding,
  kNameCountMismatch,
  kZeroSizedDescriptor,
  kDescriptorTooSmall,
  kDescriptorSizeMismatch,
  kInvalidNameIndex,
};

struct ParsedXexImportLibrary {
  // Descriptors are returned in their original wire order. Names are copied
  // without normalization, so repeated names remain repeated.
  std::string name;
  uint32_t id = 0;
  uint32_t version = 0;
  uint32_t min_version = 0;
  uint16_t name_index = 0;
  size_t descriptor_offset = 0;

  // Exact big-endian uint32_t values from the descriptor's import table,
  // converted to host endian. XEX2 consumers use these as guest addresses.
  std::vector<uint32_t> import_records;
};

struct XexImportLibraryParseResult {
  XexImportLibraryParseError error = XexImportLibraryParseError::kNone;
  size_t error_offset = 0;
  size_t bytes_consumed = 0;
  std::vector<ParsedXexImportLibrary> libraries;

  explicit operator bool() const {
    return error == XexImportLibraryParseError::kNone;
  }
};

// Parses an XEX_HEADER_IMPORT_LIBRARIES payload, whose first big-endian word
// is its size; bytes past it are ignored.
XexImportLibraryParseResult ParseXexImportLibraries(
    std::span<const uint8_t> bytes);

}  // namespace xe::cpu

#endif  // XENIA_CPU_XEX_IMPORT_LIBRARY_PARSER_H_

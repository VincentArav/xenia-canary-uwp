/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XBE_METADATA_H_
#define XENIA_XEFU_XBE_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace xe::xefu {

enum class XbeMetadataError {
  kNone,
  kFileNotFound,
  kReadFailed,
  kTooSmall,
  kBadMagic,
  kInvalidHeaderSize,
  kInvalidImageHeaderSize,
  kCertificateAddressUnderflow,
  kCertificateOverlapsImageHeader,
  kCertificateOutsideFile,
  kCertificateTooSmall,
  kCertificateTruncated,
};

enum class XbeValidationMode {
  // Requires only the bounded fields consumed by XbeMetadata.
  kMetadataOnly,
  // Also requires the standard retail certificate's fixed 0x1D0-byte body.
  kCanonicalLaunch,
};

struct XbeMetadata {
  uint32_t image_base = 0;
  uint32_t size_of_headers = 0;
  uint32_t image_header_size = 0;
  uint32_t certificate_virtual_address = 0;
  uint32_t certificate_file_offset = 0;
  uint32_t certificate_size = 0;
  uint32_t title_id = 0;
  std::u16string title_name;
};

struct XbeMetadataResult {
  XbeMetadataError error = XbeMetadataError::kNone;
  XbeMetadata metadata;

  explicit operator bool() const { return error == XbeMetadataError::kNone; }
};

XbeMetadataResult ParseXbeMetadata(
    std::span<const uint8_t> bytes,
    XbeValidationMode mode = XbeValidationMode::kMetadataOnly);
XbeMetadataResult ReadXbeMetadata(
    const std::filesystem::path& path,
    XbeValidationMode mode = XbeValidationMode::kMetadataOnly);
std::string_view XbeMetadataErrorToString(XbeMetadataError error);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XBE_METADATA_H_

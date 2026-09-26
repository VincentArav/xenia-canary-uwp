/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xbe_metadata.h"

#include <memory>
#include <optional>

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"

namespace xe::xefu {
namespace {

constexpr size_t kXbeImageBaseOffset = 0x104;
constexpr size_t kXbeSizeOfHeadersOffset = 0x108;
constexpr size_t kXbeImageHeaderSizeOffset = 0x110;
constexpr size_t kXbeCertificateAddressOffset = 0x118;
constexpr size_t kXbeFieldsEnd = 0x11C;
// Retail XBE image headers are at least 0x178 bytes; some are longer (0x184).
constexpr size_t kXbeMinimumImageHeaderSize = 0x178;

constexpr size_t kCertificateSizeOffset = 0x00;
constexpr size_t kCertificateTitleIdOffset = 0x08;
constexpr size_t kCertificateTitleNameOffset = 0x0C;
constexpr size_t kCertificateTitleNameCodeUnitCount = 40;
constexpr size_t kCertificateMetadataSize =
    kCertificateTitleNameOffset + kCertificateTitleNameCodeUnitCount * 2;
constexpr size_t kCertificateCanonicalSize = 0x1D0;

uint16_t ReadLe16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

XbeMetadataResult MakeError(XbeMetadataError error) {
  XbeMetadataResult result;
  result.error = error;
  return result;
}

}  // namespace

XbeMetadataResult ParseXbeMetadata(std::span<const uint8_t> bytes,
                                   XbeValidationMode mode) {
  if (bytes.size() < kXbeMinimumImageHeaderSize) {
    return MakeError(XbeMetadataError::kTooSmall);
  }

  if (bytes[0] != 'X' || bytes[1] != 'B' || bytes[2] != 'E' ||
      bytes[3] != 'H') {
    return MakeError(XbeMetadataError::kBadMagic);
  }

  XbeMetadataResult result;
  XbeMetadata& metadata = result.metadata;
  metadata.image_base = ReadLe32(bytes.data() + kXbeImageBaseOffset);
  metadata.size_of_headers = ReadLe32(bytes.data() + kXbeSizeOfHeadersOffset);
  metadata.image_header_size =
      ReadLe32(bytes.data() + kXbeImageHeaderSizeOffset);
  metadata.certificate_virtual_address =
      ReadLe32(bytes.data() + kXbeCertificateAddressOffset);

  if (metadata.size_of_headers < kXbeFieldsEnd ||
      metadata.size_of_headers > bytes.size()) {
    return MakeError(XbeMetadataError::kInvalidHeaderSize);
  }
  if (metadata.image_header_size < kXbeMinimumImageHeaderSize ||
      (mode == XbeValidationMode::kCanonicalLaunch &&
       (metadata.image_header_size & 3) != 0) ||
      metadata.image_header_size > metadata.size_of_headers ||
      metadata.image_header_size > bytes.size()) {
    return MakeError(XbeMetadataError::kInvalidImageHeaderSize);
  }

  if (metadata.certificate_virtual_address < metadata.image_base) {
    return MakeError(XbeMetadataError::kCertificateAddressUnderflow);
  }

  metadata.certificate_file_offset =
      metadata.certificate_virtual_address - metadata.image_base;
  const size_t certificate_offset = metadata.certificate_file_offset;
  if (certificate_offset < metadata.image_header_size) {
    return MakeError(XbeMetadataError::kCertificateOverlapsImageHeader);
  }
  if (certificate_offset > metadata.size_of_headers ||
      sizeof(uint32_t) > metadata.size_of_headers - certificate_offset) {
    return MakeError(XbeMetadataError::kCertificateOutsideFile);
  }

  metadata.certificate_size =
      ReadLe32(bytes.data() + certificate_offset + kCertificateSizeOffset);
  const size_t required_certificate_size =
      mode == XbeValidationMode::kCanonicalLaunch ? kCertificateCanonicalSize
                                                  : kCertificateMetadataSize;
  if (metadata.certificate_size < required_certificate_size) {
    return MakeError(XbeMetadataError::kCertificateTooSmall);
  }
  if (metadata.certificate_size >
      metadata.size_of_headers - certificate_offset) {
    return MakeError(XbeMetadataError::kCertificateTruncated);
  }

  metadata.title_id =
      ReadLe32(bytes.data() + certificate_offset + kCertificateTitleIdOffset);

  const uint8_t* title_name_bytes =
      bytes.data() + certificate_offset + kCertificateTitleNameOffset;
  metadata.title_name.reserve(kCertificateTitleNameCodeUnitCount);
  for (size_t i = 0; i < kCertificateTitleNameCodeUnitCount; ++i) {
    const char16_t code_unit =
        char16_t(ReadLe16(title_name_bytes + i * sizeof(uint16_t)));
    if (!code_unit) {
      break;
    }
    metadata.title_name.push_back(code_unit);
  }

  return result;
}

XbeMetadataResult ReadXbeMetadata(const std::filesystem::path& path,
                                  XbeValidationMode mode) {
  const std::optional<filesystem::FileInfo> info = filesystem::GetInfo(path);
  if (!info) {
    return MakeError(XbeMetadataError::kFileNotFound);
  }
  if (info->type != filesystem::FileInfo::Type::kFile) {
    return MakeError(XbeMetadataError::kReadFailed);
  }

  std::unique_ptr<MappedMemory> mapping =
      MappedMemory::Open(path, MappedMemory::Mode::kRead);
  if (!mapping) {
    return MakeError(XbeMetadataError::kReadFailed);
  }

  return ParseXbeMetadata(
      std::span<const uint8_t>(mapping->data(), mapping->size()), mode);
}

std::string_view XbeMetadataErrorToString(XbeMetadataError error) {
  switch (error) {
    case XbeMetadataError::kNone:
      return "none";
    case XbeMetadataError::kFileNotFound:
      return "file not found";
    case XbeMetadataError::kReadFailed:
      return "file read failed";
    case XbeMetadataError::kTooSmall:
      return "file is too small for an XBE header";
    case XbeMetadataError::kBadMagic:
      return "XBEH magic is missing";
    case XbeMetadataError::kInvalidHeaderSize:
      return "XBE header size is outside the file";
    case XbeMetadataError::kInvalidImageHeaderSize:
      return "XBE image-header size is invalid for the selected validation "
             "mode or outside the declared headers";
    case XbeMetadataError::kCertificateAddressUnderflow:
      return "certificate address is below the image base";
    case XbeMetadataError::kCertificateOverlapsImageHeader:
      return "certificate overlaps the XBE image header";
    case XbeMetadataError::kCertificateOutsideFile:
      return "certificate address is outside the declared XBE headers";
    case XbeMetadataError::kCertificateTooSmall:
      return "certificate is too small for the selected XBE validation mode";
    case XbeMetadataError::kCertificateTruncated:
      return "certificate extends beyond the declared XBE headers";
  }
  return "unknown XBE metadata error";
}

}  // namespace xe::xefu

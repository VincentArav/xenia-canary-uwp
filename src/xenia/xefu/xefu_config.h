/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_CONFIG_H_
#define XENIA_XEFU_XEFU_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "xenia/xefu/xefu_assets.h"

namespace xe::xefu {

inline constexpr uint64_t kXefuConfigSize = 0x88;

struct XefuConfigRequest {
  // Must be the absolute, exact-cased path
  // <compatibility_root>/Configs/{TITLEID:08X}.bin.
  std::filesystem::path host_path;
  XefuConfigPackFlavor source_pack = XefuConfigPackFlavor::kNone;
};

struct ValidatedXefuConfig {
  std::filesystem::path path;
  std::string canonical_guest_filename;
  XefuConfigPackFlavor source_pack = XefuConfigPackFlavor::kNone;
  uint32_t title_id = 0;
  uint64_t size = 0;
  Sha1Digest sha1{};
  Sha256Digest sha256{};
};

enum class XefuConfigError {
  kNone,
  kUnexpectedSelection,
  kInvalidRequiredPack,
  kInvalidSourcePack,
  kSourcePackMismatch,
  kPathNotAbsolute,
  kPathMismatch,
  kRootNotFound,
  kRootNotDirectory,
  kConfigsDirectoryNotFound,
  kConfigsDirectoryCaseMismatch,
  kDuplicateConfigsDirectory,
  kConfigsDirectoryNotDirectory,
  kFileNotFound,
  kFilenameCaseMismatch,
  kDuplicateFilename,
  kNotRegularFile,
  kSizeMismatch,
  kReadFailed,
};

struct XefuConfigValidationResult {
  XefuConfigError error = XefuConfigError::kNone;
  std::filesystem::path problem_path;
  std::filesystem::path expected_path;
  XefuConfigPackFlavor expected_source_pack = XefuConfigPackFlavor::kNone;
  XefuConfigPackFlavor actual_source_pack = XefuConfigPackFlavor::kNone;
  uint64_t expected_size = kXefuConfigSize;
  uint64_t actual_size = 0;
  ValidatedXefuConfig config;

  explicit operator bool() const { return error == XefuConfigError::kNone; }
};

// Produces the only config filename accepted for a title, with eight uppercase
// hexadecimal digits and a lowercase .bin extension.
std::string XefuConfigFilenameForTitleId(uint32_t title_id);

// Validates a config selection and the file's identity. pack is the pack a
// build's config must come from, or none when the build takes no config, in
// which case a supplied config is rejected. Omitting the config is valid.
XefuConfigValidationResult ValidateXefuConfig(
    const std::filesystem::path& compatibility_root, uint32_t title_id,
    const std::optional<XefuConfigRequest>& request,
    const std::optional<XefuConfigPackFlavor>& required_pack);

std::string_view XefuConfigErrorToString(XefuConfigError error);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_CONFIG_H_

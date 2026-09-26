/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_config.h"

#include <array>
#include <system_error>

#include "xenia/base/filesystem.h"
#include "xenia/base/utf8.h"

namespace xe::xefu {
namespace {

enum class ExactEntryError {
  kNone,
  kNotFound,
  kCaseMismatch,
  kDuplicate,
  kReadFailed,
};

struct ExactEntryResult {
  ExactEntryError error = ExactEntryError::kNone;
  std::filesystem::path path;
};

XefuConfigValidationResult MakeError(XefuConfigError error,
                                     const std::filesystem::path& problem_path,
                                     const std::filesystem::path& expected_path,
                                     XefuConfigPackFlavor expected_source_pack,
                                     XefuConfigPackFlavor actual_source_pack) {
  XefuConfigValidationResult result;
  result.error = error;
  result.problem_path = problem_path;
  result.expected_path = expected_path;
  result.expected_source_pack = expected_source_pack;
  result.actual_source_pack = actual_source_pack;
  return result;
}

ExactEntryResult FindExactEntry(const std::filesystem::path& directory,
                                std::string_view expected_name) {
  ExactEntryResult result;
  const std::string folded_expected = utf8::lower_ascii(expected_name);
  size_t matching_entries = 0;
  bool found_exact = false;

  std::error_code iterator_error;
  std::filesystem::directory_iterator iterator(directory, iterator_error);
  const std::filesystem::directory_iterator end;
  if (iterator_error) {
    result.error = ExactEntryError::kReadFailed;
    return result;
  }
  while (iterator != end) {
    const std::filesystem::path candidate_name = iterator->path().filename();
    if (utf8::lower_ascii(path_to_utf8(candidate_name)) == folded_expected) {
      ++matching_entries;
      if (candidate_name == to_path(expected_name)) {
        found_exact = true;
        result.path = iterator->path();
      }
    }
    iterator.increment(iterator_error);
    if (iterator_error) {
      result.error = ExactEntryError::kReadFailed;
      return result;
    }
  }

  if (matching_entries > 1) {
    result.error = ExactEntryError::kDuplicate;
  } else if (!matching_entries) {
    result.error = ExactEntryError::kNotFound;
  } else if (!found_exact) {
    result.error = ExactEntryError::kCaseMismatch;
  }
  return result;
}

bool IsSafeDirectory(const std::filesystem::path& path, bool* missing) {
  *missing = false;
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory ||
      (!status_error &&
       status.type() == std::filesystem::file_type::not_found)) {
    *missing = true;
    return false;
  }
  if (status_error || std::filesystem::is_symlink(status) ||
      filesystem::IsSymlinkOrReparsePoint(path) ||
      !std::filesystem::is_directory(status)) {
    return false;
  }
  const std::optional<filesystem::FileInfo> info = filesystem::GetInfo(path);
  return info && info->type == filesystem::FileInfo::Type::kDirectory &&
         !info->is_symlink_or_reparse_point;
}

bool IsSafeRegularFile(const std::filesystem::path& path) {
  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, status_error);
  if (status_error || std::filesystem::is_symlink(status) ||
      filesystem::IsSymlinkOrReparsePoint(path) ||
      !std::filesystem::is_regular_file(status)) {
    return false;
  }
  const std::optional<filesystem::FileInfo> info = filesystem::GetInfo(path);
  return info && info->type == filesystem::FileInfo::Type::kFile &&
         !info->is_symlink_or_reparse_point;
}

bool IsSelectableConfigPack(XefuConfigPackFlavor flavor) {
  return flavor == XefuConfigPackFlavor::kModified ||
         flavor == XefuConfigPackFlavor::kModifiedWithHud;
}

}  // namespace

std::string XefuConfigFilenameForTitleId(uint32_t title_id) {
  constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5',
                                            '6', '7', '8', '9', 'A', 'B',
                                            'C', 'D', 'E', 'F'};
  std::string filename(12, '0');
  for (size_t i = 0; i < 8; ++i) {
    filename[i] = kHexDigits[(title_id >> ((7 - i) * 4)) & 0xF];
  }
  filename[8] = '.';
  filename[9] = 'b';
  filename[10] = 'i';
  filename[11] = 'n';
  return filename;
}

XefuConfigValidationResult ValidateXefuConfig(
    const std::filesystem::path& compatibility_root, uint32_t title_id,
    const std::optional<XefuConfigRequest>& request,
    const std::optional<XefuConfigPackFlavor>& required_pack) {
  const XefuConfigPackFlavor expected_source_pack =
      required_pack.value_or(XefuConfigPackFlavor::kNone);
  const XefuConfigPackFlavor actual_source_pack =
      request ? request->source_pack : XefuConfigPackFlavor::kNone;
  const std::string canonical_filename = XefuConfigFilenameForTitleId(title_id);
  const std::filesystem::path normalized_root =
      compatibility_root.lexically_normal();
  const std::filesystem::path expected_path =
      normalized_root / "Configs" / to_path(canonical_filename);

  if (required_pack && !IsSelectableConfigPack(*required_pack)) {
    return MakeError(XefuConfigError::kInvalidRequiredPack, compatibility_root,
                     expected_path, expected_source_pack, actual_source_pack);
  }
  if (!required_pack) {
    if (request) {
      return MakeError(XefuConfigError::kUnexpectedSelection,
                       request->host_path, expected_path, expected_source_pack,
                       actual_source_pack);
    }
    XefuConfigValidationResult result;
    result.expected_path = expected_path;
    return result;
  }
  if (!request) {
    XefuConfigValidationResult result;
    result.expected_path = expected_path;
    result.expected_source_pack = expected_source_pack;
    return result;
  }
  if (!IsSelectableConfigPack(request->source_pack)) {
    return MakeError(XefuConfigError::kInvalidSourcePack, request->host_path,
                     expected_path, expected_source_pack, actual_source_pack);
  }
  if (request->source_pack != *required_pack) {
    return MakeError(XefuConfigError::kSourcePackMismatch, request->host_path,
                     expected_path, expected_source_pack, actual_source_pack);
  }
  if (!request->host_path.is_absolute()) {
    return MakeError(XefuConfigError::kPathNotAbsolute, request->host_path,
                     expected_path, expected_source_pack, actual_source_pack);
  }
  if (compatibility_root != normalized_root ||
      request->host_path != request->host_path.lexically_normal() ||
      request->host_path != expected_path) {
    return MakeError(XefuConfigError::kPathMismatch, request->host_path,
                     expected_path, expected_source_pack, actual_source_pack);
  }

  bool root_missing = false;
  if (!IsSafeDirectory(normalized_root, &root_missing)) {
    return MakeError(root_missing ? XefuConfigError::kRootNotFound
                                  : XefuConfigError::kRootNotDirectory,
                     normalized_root, expected_path, expected_source_pack,
                     actual_source_pack);
  }

  const ExactEntryResult configs_entry =
      FindExactEntry(normalized_root, "Configs");
  switch (configs_entry.error) {
    case ExactEntryError::kNone:
      break;
    case ExactEntryError::kNotFound:
      return MakeError(XefuConfigError::kConfigsDirectoryNotFound,
                       normalized_root / "Configs", expected_path,
                       expected_source_pack, actual_source_pack);
    case ExactEntryError::kCaseMismatch:
      return MakeError(XefuConfigError::kConfigsDirectoryCaseMismatch,
                       normalized_root / "Configs", expected_path,
                       expected_source_pack, actual_source_pack);
    case ExactEntryError::kDuplicate:
      return MakeError(XefuConfigError::kDuplicateConfigsDirectory,
                       normalized_root / "Configs", expected_path,
                       expected_source_pack, actual_source_pack);
    case ExactEntryError::kReadFailed:
      return MakeError(XefuConfigError::kReadFailed, normalized_root,
                       expected_path, expected_source_pack, actual_source_pack);
  }

  bool configs_missing = false;
  if (configs_entry.path != normalized_root / "Configs" ||
      !IsSafeDirectory(configs_entry.path, &configs_missing)) {
    return MakeError(XefuConfigError::kConfigsDirectoryNotDirectory,
                     configs_entry.path, expected_path, expected_source_pack,
                     actual_source_pack);
  }

  const ExactEntryResult config_entry =
      FindExactEntry(configs_entry.path, canonical_filename);
  switch (config_entry.error) {
    case ExactEntryError::kNone:
      break;
    case ExactEntryError::kNotFound:
      return MakeError(XefuConfigError::kFileNotFound, expected_path,
                       expected_path, expected_source_pack, actual_source_pack);
    case ExactEntryError::kCaseMismatch:
      return MakeError(XefuConfigError::kFilenameCaseMismatch, expected_path,
                       expected_path, expected_source_pack, actual_source_pack);
    case ExactEntryError::kDuplicate:
      return MakeError(XefuConfigError::kDuplicateFilename, expected_path,
                       expected_path, expected_source_pack, actual_source_pack);
    case ExactEntryError::kReadFailed:
      return MakeError(XefuConfigError::kReadFailed, configs_entry.path,
                       expected_path, expected_source_pack, actual_source_pack);
  }

  if (config_entry.path != expected_path ||
      !IsSafeRegularFile(config_entry.path)) {
    return MakeError(XefuConfigError::kNotRegularFile, config_entry.path,
                     expected_path, expected_source_pack, actual_source_pack);
  }

  // The size and both digests come from one read.
  XefuFileDigests digests;
  if (!ComputeFileDigests(config_entry.path, &digests)) {
    return MakeError(XefuConfigError::kReadFailed, config_entry.path,
                     expected_path, expected_source_pack, actual_source_pack);
  }
  if (digests.size != kXefuConfigSize) {
    XefuConfigValidationResult result =
        MakeError(XefuConfigError::kSizeMismatch, config_entry.path,
                  expected_path, expected_source_pack, actual_source_pack);
    result.actual_size = digests.size;
    return result;
  }

  // Recheck the path after reading; mounting checks it again.
  if (!IsSafeRegularFile(config_entry.path)) {
    return MakeError(XefuConfigError::kNotRegularFile, config_entry.path,
                     expected_path, expected_source_pack, actual_source_pack);
  }

  XefuConfigValidationResult result;
  result.expected_path = expected_path;
  result.expected_source_pack = expected_source_pack;
  result.actual_source_pack = actual_source_pack;
  result.actual_size = digests.size;
  result.config.path = config_entry.path;
  result.config.canonical_guest_filename = canonical_filename;
  result.config.source_pack = request->source_pack;
  result.config.title_id = title_id;
  result.config.size = digests.size;
  result.config.sha1 = digests.sha1;
  result.config.sha256 = digests.sha256;
  return result;
}

std::string_view XefuConfigErrorToString(XefuConfigError error) {
  switch (error) {
    case XefuConfigError::kNone:
      return "none";
    case XefuConfigError::kUnexpectedSelection:
      return "this XeFu build does not accept an external config";
    case XefuConfigError::kInvalidRequiredPack:
      return "the XeFu build has an invalid external-config requirement";
    case XefuConfigError::kInvalidSourcePack:
      return "the selected external config has an invalid source pack";
    case XefuConfigError::kSourcePackMismatch:
      return "the external config is from a different XeFu pack flavor";
    case XefuConfigError::kPathNotAbsolute:
      return "the external-config host path is not absolute";
    case XefuConfigError::kPathMismatch:
      return "the external-config path is not the exact canonical path";
    case XefuConfigError::kRootNotFound:
      return "the external-config compatibility root was not found";
    case XefuConfigError::kRootNotDirectory:
      return "the external-config compatibility root is not a safe directory";
    case XefuConfigError::kConfigsDirectoryNotFound:
      return "the Configs directory was not found";
    case XefuConfigError::kConfigsDirectoryCaseMismatch:
      return "the Configs directory does not have its exact canonical case";
    case XefuConfigError::kDuplicateConfigsDirectory:
      return "multiple case-insensitive Configs directories exist";
    case XefuConfigError::kConfigsDirectoryNotDirectory:
      return "the Configs path is not a safe directory";
    case XefuConfigError::kFileNotFound:
      return "the title's external-config file was not found";
    case XefuConfigError::kFilenameCaseMismatch:
      return "the external-config filename does not have canonical case";
    case XefuConfigError::kDuplicateFilename:
      return "multiple case-insensitive external-config filenames exist";
    case XefuConfigError::kNotRegularFile:
      return "the external config is not a safe regular file";
    case XefuConfigError::kSizeMismatch:
      return "the external config is not exactly 0x88 bytes";
    case XefuConfigError::kReadFailed:
      return "the external config could not be enumerated or read";
  }
  return "unknown XeFu external-config error";
}

}  // namespace xe::xefu

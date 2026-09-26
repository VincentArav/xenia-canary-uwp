/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/filesystem.h"

#include "xenia/base/platform.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"

namespace xe {
namespace filesystem {

namespace {

std::string HostPathOverlapKey(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
  }
  std::string key = path_to_utf8(absolute.lexically_normal());
#if XE_PLATFORM_WIN32
  key = utf8::lower_ascii(key);
#endif
  while (key.size() > 1 && (key.back() == '\\' || key.back() == '/')) {
    key.pop_back();
  }
  return key;
}

bool IsHostPathKeyParentOrEqual(std::string_view parent,
                                std::string_view child) {
  return !parent.empty() && child.starts_with(parent) &&
         (child.size() == parent.size() || parent.back() == '\\' ||
          parent.back() == '/' || child[parent.size()] == '\\' ||
          child[parent.size()] == '/');
}

}  // namespace

bool HostPathsOverlap(const std::filesystem::path& left,
                      const std::filesystem::path& right) {
  const std::string left_key = HostPathOverlapKey(left);
  const std::string right_key = HostPathOverlapKey(right);
  return IsHostPathKeyParentOrEqual(left_key, right_key) ||
         IsHostPathKeyParentOrEqual(right_key, left_key);
}

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

std::error_code CreateFolder(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) {
    return {};
  }

  std::error_code ec;
  if (std::filesystem::create_directories(path, ec)) {
    return {};
  }

  return ec;
}

std::vector<FileInfo> ListDirectories(const std::filesystem::path& path) {
  std::vector<FileInfo> files = ListFiles(path);
  std::vector<FileInfo> directories = {};

  std::copy_if(files.cbegin(), files.cend(), std::back_inserter(directories),
               [](const FileInfo& file) {
                 return file.type == FileInfo::Type::kDirectory;
               });

  return directories;
}

std::vector<FileInfo> FilterByName(const std::vector<FileInfo>& files,
                                   const std::regex pattern) {
  std::vector<FileInfo> filtered_entries = {};

  std::copy_if(
      files.cbegin(), files.cend(), std::back_inserter(filtered_entries),
      [pattern](const FileInfo& file) {
        return std::regex_match(file.name.filename().string(), pattern);
      });
  return filtered_entries;
}

std::vector<FileInfo> FindFileWithName(const std::filesystem::path& path,
                                       std::string_view name, bool recursive) {
  if (!std::filesystem::exists(path)) {
    return {};
  }

  if (!std::filesystem::is_directory(path)) {
    return {};
  }

  if (!recursive) {
    return FilterByName(ListFiles(path), std::regex(std::string(name)));
  }

  const std::string file_name = xe::utf8::lower_ascii(name);

  std::vector<FileInfo> filtered_entries = {};
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_regular_file() && xe::utf8::lower_ascii(xe::path_to_utf8(
                                       entry.path().filename())) == file_name) {
      auto file_info = GetInfo(entry.path());
      if (file_info) {
        filtered_entries.push_back(std::move(file_info.value()));
      }
    }
  }
  return filtered_entries;
}

}  // namespace filesystem
}  // namespace xe

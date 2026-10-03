/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xna_indie.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "xenia/base/filesystem.h"

namespace xe::xefu {
namespace {

static_assert(kIndieLaunchDataFileNameOffset +
                  kIndieLaunchDataFileNameMaxSize <=
              kIndieLaunchDataDeviceIdOffset);
static_assert(kIndieLaunchDataDeviceIdOffset + sizeof(uint32_t) ==
              kIndieLaunchDataSize);

void StoreBe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24);
  bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8);
  bytes[3] = uint8_t(value);
}

// Characters no host folder or guest path element may contain.
constexpr std::string_view kReservedNameCharacters = "\\/:*?\"<>|";

// The content manager uses the name as a host folder and a guest path element
// as given, and trims spaces from it; Windows drops a trailing dot.
bool IsUsableInstallName(std::string_view name) {
  if (name.empty() || name.size() > kIndieLaunchDataFileNameMaxSize ||
      name == "." || name == "..") {
    return false;
  }
  if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return c >= 0x20 && c <= 0x7E &&
           kReservedNameCharacters.find(c) == std::string_view::npos;
  });
}

}  // namespace

bool IsIndieGamePackageHeader(XContentType content_type, uint32_t title_id) {
  return content_type == XContentType::kMarketplaceContent &&
         title_id == kXnaIndiePlayerTitleId;
}

bool IsIndieGamePackageHeader(const vfs::XContentContainerHeader& header) {
  return header.content_header.is_magic_valid() &&
         IsIndieGamePackageHeader(
             header.content_metadata.content_type.get(),
             header.content_metadata.execution_info.title_id.get());
}

std::unique_ptr<vfs::XContentContainerHeader> ReadIndieGamePackageHeader(
    const std::filesystem::path& package_path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(package_path, error) || error) {
    return nullptr;
  }
  const std::uintmax_t file_size =
      std::filesystem::file_size(package_path, error);
  if (error || file_size < sizeof(vfs::XContentContainerHeader)) {
    return nullptr;
  }
  FILE* file = xe::filesystem::OpenFile(package_path, "rb");
  if (!file) {
    return nullptr;
  }
  auto header = std::make_unique<vfs::XContentContainerHeader>();
  const bool read = std::fread(header.get(), sizeof(*header), 1, file) == 1;
  std::fclose(file);
  if (!read || !IsIndieGamePackageHeader(*header)) {
    return nullptr;
  }
  return header;
}

bool IsIndieGamePackage(const std::filesystem::path& package_path) {
  return ReadIndieGamePackageHeader(package_path) != nullptr;
}

std::string IndieGamePackageInstallName(
    const vfs::XContentContainerHeader& header,
    std::string_view package_file_name) {
  constexpr size_t kContentIdSize = sizeof(vfs::XContentHeader::content_id);
  static_assert(kContentIdSize * 2 <= kIndieLaunchDataFileNameMaxSize);
  const uint8_t* const content_id = header.content_header.content_id;
  if (std::any_of(content_id, content_id + kContentIdSize,
                  [](uint8_t byte) { return byte != 0; })) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string name;
    name.reserve(kContentIdSize * 2);
    for (size_t i = 0; i < kContentIdSize; ++i) {
      name.push_back(kHexDigits[content_id[i] >> 4]);
      name.push_back(kHexDigits[content_id[i] & 0xF]);
    }
    return name;
  }
  if (IsUsableInstallName(package_file_name)) {
    return std::string(package_file_name);
  }
  return {};
}

IndieLaunchData BuildIndieLaunchData(std::string_view package_file_name,
                                     uint32_t device_id) {
  IndieLaunchData data{};
  StoreBe32(data.data(), kIndieLaunchDataMagic);
  const size_t name_size =
      std::min(package_file_name.size(), kIndieLaunchDataFileNameMaxSize);
  if (name_size) {
    std::memcpy(data.data() + kIndieLaunchDataFileNameOffset,
                package_file_name.data(), name_size);
  }
  StoreBe32(data.data() + kIndieLaunchDataDeviceIdOffset, device_id);
  return data;
}

}  // namespace xe::xefu

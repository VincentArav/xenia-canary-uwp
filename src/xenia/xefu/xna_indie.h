/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XNA_INDIE_H_
#define XENIA_XEFU_XNA_INDIE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "xenia/kernel/title_id_utils.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/xbox.h"

namespace xe::xefu {

// An Xbox Live Indie Game is not a title of its own. It ships as a LIVE
// package of marketplace content owned by the XNA Indie Player
// (Xna_TitleLauncher.xex, from the 360 system update), and the player runs
// the managed game inside it.
constexpr uint32_t kXnaIndiePlayerTitleId = kernel::kXN_2002;

// The launch data the player reads with XamLoaderGetLaunchData to find its
// package (layout from xenia-edge, unverified on a console):
//   +0x00  0xCAFEBABE, big-endian
//   +0x04  the package's file name (ASCII), as XCONTENT_DATA holds it: at most
//          0x2A bytes, not necessarily NUL-terminated; zero up to +0x30
//   +0x30  the ID of the device holding the package, big-endian
constexpr size_t kIndieLaunchDataSize = 0x34;
constexpr uint32_t kIndieLaunchDataMagic = 0xCAFEBABE;
constexpr size_t kIndieLaunchDataFileNameOffset = 0x04;
constexpr size_t kIndieLaunchDataFileNameMaxSize = 0x2A;
constexpr size_t kIndieLaunchDataDeviceIdOffset = 0x30;

using IndieLaunchData = std::array<uint8_t, kIndieLaunchDataSize>;

// Whether a package of this content type and title is an indie game.
bool IsIndieGamePackageHeader(XContentType content_type, uint32_t title_id);
// The same for a whole header, which must also carry CON, LIVE or PIRS magic.
bool IsIndieGamePackageHeader(const vfs::XContentContainerHeader& header);

// Reads only a package's header. Null unless the file is an indie game
// package (including a file too short to hold the header).
std::unique_ptr<vfs::XContentContainerHeader> ReadIndieGamePackageHeader(
    const std::filesystem::path& package_path);
bool IsIndieGamePackage(const std::filesystem::path& package_path);

// The name an indie game package is mounted as and the launch data carries:
// its content ID in hex, or its file name when the ID is blank. Installed
// content is found in a host folder of that name. Empty when neither is a
// usable folder name that fits XCONTENT_DATA (1 to 0x2A printable ASCII bytes,
// none of \/:*?"<>|, no leading or trailing space, no trailing dot).
std::string IndieGamePackageInstallName(
    const vfs::XContentContainerHeader& header,
    std::string_view package_file_name);

// Longer names are truncated to kIndieLaunchDataFileNameMaxSize bytes, as
// XCONTENT_DATA would store them.
IndieLaunchData BuildIndieLaunchData(std::string_view package_file_name,
                                     uint32_t device_id);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XNA_INDIE_H_

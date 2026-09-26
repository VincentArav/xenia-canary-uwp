/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_ASSETS_H_
#define XENIA_XEFU_XEFU_ASSETS_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xe::xefu {

using Sha1Digest = std::array<uint8_t, 20>;
using Sha256Digest = std::array<uint8_t, 32>;

enum class XefuBuildId {
  kRetailUnpatched,
  kRetailPatched,
  kRetail2,
  kRetail3,
  kRetail5,
  kRetail1_1,
  kRetail6,
  kRetail7,
  kRetail7b,
  kHackedPatched,
  kHacked1_1,
  kHacked2,
  kHacked3,
  kHacked5,
  kHacked6,
  kHacked7,
  kHacked7b,
  kHackedConfigLoader7,
  kHacked2019,
  kHacked2021a,
  kHacked2021b,
  kHacked2021c,
  kHackedHudPatched,
  kHackedHud1_1,
  kHackedHud2,
  kHackedHud3,
  kHackedHud5,
  kHackedHud6,
  kHackedHud7,
  kHackedHud7b,
  kHackedHudConfigLoader7,
  kHackedHud2019,
  kHackedHud2021a,
  kHackedHud2021b,
  kHackedHud2021c,
};

enum class XefuAssetRole {
  kRuntime,
  kTitleModule,
  kSelector,
  kResource,
};

enum class XefuAssetProvenance {
  kRetail,
  kModified,
  kModifiedWithHud,
};

// The community pack an external XeFu configuration comes from. Most XeFu
// builds accept no external configuration.
enum class XefuConfigPackFlavor : uint8_t {
  kNone = 0,
  kModified = 1,
  kModifiedWithHud = 2,
};

struct XefuAssetRequirement {
  XefuAssetRole role = XefuAssetRole::kResource;
  std::string_view filename;
  // The name guest imports and file lookups use; may differ from filename for
  // community-named assets such as later xefutitle builds.
  std::string_view guest_filename;
  Sha1Digest sha1{};
  // Zero when the size isn't checked.
  uint64_t size = 0;
  bool required = true;
  // When present, the file's SHA-256 must match too.
  std::optional<Sha256Digest> sha256;
};

struct XefuBuildCapabilities {
  // XBE, asset and launch-data preparation and session mounting.
  bool preparation_supported = false;
  // Running the build; launching also needs the checked-memory backend and the
  // kernel user-mode runtime.
  bool execution_supported = false;
};

struct XefuBuildDescriptor {
  XefuBuildId id = XefuBuildId::kRetailUnpatched;
  std::string_view display_name;
  XefuAssetProvenance provenance = XefuAssetProvenance::kRetail;
  std::span<const XefuAssetRequirement> requirements;
  XefuBuildCapabilities capabilities;
  // Set only for runtimes that can load a 0x88-byte title configuration: a
  // selected config must come from this pack. A config is optional; without
  // one the runtime uses its xefutitle companion.
  std::optional<XefuConfigPackFlavor> required_external_config_pack;
};

enum class XefuAssetError {
  kNone,
  kInvalidManifest,
  kRootNotFound,
  kRootNotDirectory,
  kMissingFile,
  kDuplicateFilename,
  kNotRegularFile,
  kSizeMismatch,
  kReadFailed,
  kHashMismatch,
  kUnsupportedBuild,
};

struct ValidatedXefuAsset {
  XefuAssetRole role = XefuAssetRole::kResource;
  std::filesystem::path path;
  std::string guest_filename;
  Sha1Digest sha1{};
  uint64_t size = 0;
  std::optional<Sha256Digest> sha256;
};

struct ValidatedXefuAssetSet {
  XefuBuildId build = XefuBuildId::kRetailUnpatched;
  std::filesystem::path root;
  std::vector<ValidatedXefuAsset> assets;

  const ValidatedXefuAsset* Find(XefuAssetRole role) const;
};

struct XefuAssetValidationResult {
  XefuAssetError error = XefuAssetError::kNone;
  std::filesystem::path problem_path;
  Sha1Digest expected_sha1{};
  Sha1Digest actual_sha1{};
  std::optional<Sha256Digest> expected_sha256;
  std::optional<Sha256Digest> actual_sha256;
  uint64_t expected_size = 0;
  uint64_t actual_size = 0;
  ValidatedXefuAssetSet assets;

  explicit operator bool() const { return error == XefuAssetError::kNone; }
};

struct XefuFileDigests {
  Sha1Digest sha1{};
  Sha256Digest sha256{};
  // Bytes hashed.
  uint64_t size = 0;
};

// Computes both hashes in one read of one open file.
bool ComputeFileDigests(const std::filesystem::path& path,
                        XefuFileDigests* digests);

const XefuBuildDescriptor* FindKnownXefuBuild(XefuBuildId id);

XefuAssetValidationResult ValidateXefuAssets(
    const std::filesystem::path& root, const XefuBuildDescriptor& descriptor);
XefuAssetValidationResult ValidateKnownXefuAssets(
    const std::filesystem::path& root, XefuBuildId build);

std::string_view XefuAssetErrorToString(XefuAssetError error);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_ASSETS_H_

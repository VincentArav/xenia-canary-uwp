/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_SELECTION_H_
#define XENIA_XEFU_XEFU_SELECTION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "xenia/xefu/xefu_assets.h"

namespace xe::xefu {

// One row of an xbox.xex selector table.
struct XefuOfficialSelectorEntry {
  uint32_t title_id = 0;
  uint32_t live_flag = 0;
  std::string_view emulator_name;
  uint32_t allowed_flag = 0;
};

enum class XefuBuildSelectionSource {
  kOfficialSelectorTable,
  kManualExactOverride,
};

enum class XefuBuildSelectionDiagnostic {
  kNone,
  kTitleNotListed,
  kTitleDisallowed,
  kAmbiguousTitle,
  kInvalidTableEntry,
  kUnknownEmulatorName,
  kInvalidManualOverride,
  kBuildNotPreparationSupported,
};

struct XefuBuildSelection {
  XefuBuildId build = XefuBuildId::kRetailUnpatched;
  uint32_t live_enabled = 0;
  XefuBuildSelectionSource source =
      XefuBuildSelectionSource::kOfficialSelectorTable;
};

struct XefuBuildSelectionResult {
  XefuBuildSelectionDiagnostic diagnostic = XefuBuildSelectionDiagnostic::kNone;
  size_t matching_row_count = 0;
  std::optional<XefuBuildSelection> selection;

  // A selection doesn't permit running the build.
  static constexpr bool execution_permitted = false;

  explicit operator bool() const {
    return diagnostic == XefuBuildSelectionDiagnostic::kNone &&
           selection.has_value();
  }
};

// Maps the eight retail emulator names in the 5832 selector table. `xefu` is
// the retail image as patched in November 2005.
std::optional<XefuBuildId> XefuRetailBuildForOfficialEmulatorName(
    std::string_view emulator_name);

// Case-insensitive command-line names, one per known build.
std::optional<XefuBuildId> XefuBuildForCommandLineName(std::string_view name);
std::string_view XefuBuildCommandLineName(XefuBuildId build);

// Selects from a selector table only when every row for the title agrees on
// whether it is allowed and, if so, on the build and Live flag.
XefuBuildSelectionResult SelectOfficialXefuBuild(
    uint32_t title_id, std::span<const XefuOfficialSelectorEntry> entries);

// Selects a known build directly; fails for one that can't be prepared.
XefuBuildSelectionResult SelectManualXefuBuild(XefuBuildId build,
                                               uint32_t live_enabled);

// What a known build's modifications allow or require.
struct XefuRestrictionProfile {
  bool selector_allowlist_bypassed = false;
  bool debug_xbe_keys_supported = false;
  bool runtime_internal_validation_bypassed = false;
  bool external_config_overlay_supported = false;
  bool companion_required = false;
};

std::optional<XefuRestrictionProfile> GetXefuRestrictionProfile(
    XefuBuildId build);

std::string_view XefuBuildSelectionDiagnosticToString(
    XefuBuildSelectionDiagnostic diagnostic);

// One XeFu release, whichever pack supplies it.
enum class XefuGeneration : uint8_t {
  kNovember2005,  // xefu.xex as patched in November 2005
  k1_1,
  k2,
  k3,
  k5,
  k6,
  k7,
  k7b,
  // Builds found in the Xbox One / Series releases of particular titles; only
  // the modified packs carry them.
  k2019,
  k2021a,
  k2021b,
  k2021c,
};

// The exact build of a release in a pack, if the pack has it.
std::optional<XefuBuildId> XefuBuildForGeneration(XefuGeneration generation,
                                                  XefuAssetProvenance pack);
// The pack that supplies an exact build.
XefuAssetProvenance XefuPackForBuild(XefuBuildId build);
// The release the official Xbox 360 selector chooses for a title, if listed.
std::optional<XefuGeneration> OfficialXefuGenerationForTitle(uint32_t title_id);

enum class XefuAutomaticReason {
  // Microsoft shipped a newer XeFu with this title's Xbox One release.
  kXboxOneRelease,
  // A community config for the title is in <pack>/Configs; the config loader
  // (a release 7 build) applies it.
  kCommunityConfig,
  // Only the Xbox One-era title tables (2019/2021) carry settings for it.
  kXboxOneTitleTable,
  // The official Xbox 360 selector's choice.
  kOfficialList,
  // Unlisted: the latest Xbox 360 release, as a modified console uses.
  kLatestRelease,
};

struct XefuAutomaticSelection {
  XefuBuildId build = XefuBuildId::kHacked7b;
  XefuAutomaticReason reason = XefuAutomaticReason::kLatestRelease;
};

// Picks the build for a title from a pack. community_config_available means
// <pack>/Configs/{TITLEID:08X}.bin exists; it is used only with a modified
// pack, for titles that are unlisted or officially on release 7 or 7b.
XefuAutomaticSelection SelectAutomaticXefuBuild(
    uint32_t title_id, XefuAssetProvenance pack,
    bool community_config_available);
std::string_view XefuAutomaticReasonToString(XefuAutomaticReason reason);

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_SELECTION_H_

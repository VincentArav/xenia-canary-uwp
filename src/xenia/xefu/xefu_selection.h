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

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_SELECTION_H_

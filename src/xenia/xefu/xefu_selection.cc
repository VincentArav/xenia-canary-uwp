/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_selection.h"

#include <algorithm>
#include <array>

namespace xe::xefu {
namespace {

struct OfficialEmulatorName {
  std::string_view name;
  XefuBuildId build;
};

constexpr std::array<OfficialEmulatorName, 8> kOfficialEmulatorNames{{
    {"xefu", XefuBuildId::kRetailPatched},
    {"xefu1_1", XefuBuildId::kRetail1_1},
    {"xefu2", XefuBuildId::kRetail2},
    {"xefu3", XefuBuildId::kRetail3},
    {"xefu5", XefuBuildId::kRetail5},
    {"xefu6", XefuBuildId::kRetail6},
    {"xefu7", XefuBuildId::kRetail7},
    {"xefu7b", XefuBuildId::kRetail7b},
}};

constexpr std::array<OfficialEmulatorName, 35> kCommandLineBuildNames{{
    {"retail-september-2005", XefuBuildId::kRetailUnpatched},
    {"retail-november-2005", XefuBuildId::kRetailPatched},
    {"retail2", XefuBuildId::kRetail2},
    {"retail3", XefuBuildId::kRetail3},
    {"retail5", XefuBuildId::kRetail5},
    {"retail1_1", XefuBuildId::kRetail1_1},
    {"retail6", XefuBuildId::kRetail6},
    {"retail7", XefuBuildId::kRetail7},
    {"retail7b", XefuBuildId::kRetail7b},
    {"hacked-november-2005", XefuBuildId::kHackedPatched},
    {"hacked1_1", XefuBuildId::kHacked1_1},
    {"hacked2", XefuBuildId::kHacked2},
    {"hacked3", XefuBuildId::kHacked3},
    {"hacked5", XefuBuildId::kHacked5},
    {"hacked6", XefuBuildId::kHacked6},
    {"hacked7", XefuBuildId::kHacked7},
    {"hacked7b", XefuBuildId::kHacked7b},
    {"hacked-config-loader7", XefuBuildId::kHackedConfigLoader7},
    {"hacked2019", XefuBuildId::kHacked2019},
    {"hacked2021a", XefuBuildId::kHacked2021a},
    {"hacked2021b", XefuBuildId::kHacked2021b},
    {"hacked2021c", XefuBuildId::kHacked2021c},
    {"hacked-hud-november-2005", XefuBuildId::kHackedHudPatched},
    {"hacked-hud1_1", XefuBuildId::kHackedHud1_1},
    {"hacked-hud2", XefuBuildId::kHackedHud2},
    {"hacked-hud3", XefuBuildId::kHackedHud3},
    {"hacked-hud5", XefuBuildId::kHackedHud5},
    {"hacked-hud6", XefuBuildId::kHackedHud6},
    {"hacked-hud7", XefuBuildId::kHackedHud7},
    {"hacked-hud7b", XefuBuildId::kHackedHud7b},
    {"hacked-hud-config-loader7", XefuBuildId::kHackedHudConfigLoader7},
    {"hacked-hud2019", XefuBuildId::kHackedHud2019},
    {"hacked-hud2021a", XefuBuildId::kHackedHud2021a},
    {"hacked-hud2021b", XefuBuildId::kHackedHud2021b},
    {"hacked-hud2021c", XefuBuildId::kHackedHud2021c},
}};

bool AsciiCaseEqual(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    const char left_char =
        left[i] >= 'A' && left[i] <= 'Z' ? char(left[i] - 'A' + 'a') : left[i];
    const char right_char = right[i] >= 'A' && right[i] <= 'Z'
                                ? char(right[i] - 'A' + 'a')
                                : right[i];
    if (left_char != right_char) {
      return false;
    }
  }
  return true;
}

bool HasVerifiedInternalValidationBypass(XefuBuildId build) {
  switch (build) {
    case XefuBuildId::kHackedPatched:
    case XefuBuildId::kHacked1_1:
    case XefuBuildId::kHacked2:
    case XefuBuildId::kHacked3:
    case XefuBuildId::kHacked5:
    case XefuBuildId::kHacked6:
    case XefuBuildId::kHacked7:
    case XefuBuildId::kHacked7b:
    case XefuBuildId::kHackedConfigLoader7:
    case XefuBuildId::kHackedHudPatched:
    case XefuBuildId::kHackedHud1_1:
    case XefuBuildId::kHackedHud2:
    case XefuBuildId::kHackedHud3:
    case XefuBuildId::kHackedHud5:
    case XefuBuildId::kHackedHud6:
    case XefuBuildId::kHackedHud7:
    case XefuBuildId::kHackedHud7b:
    case XefuBuildId::kHackedHudConfigLoader7:
      return true;
    default:
      return false;
  }
}

bool IsVerifiedModifiedBuild(XefuBuildId build) {
  switch (build) {
    case XefuBuildId::kHackedPatched:
    case XefuBuildId::kHacked1_1:
    case XefuBuildId::kHacked2:
    case XefuBuildId::kHacked3:
    case XefuBuildId::kHacked5:
    case XefuBuildId::kHacked6:
    case XefuBuildId::kHacked7:
    case XefuBuildId::kHacked7b:
    case XefuBuildId::kHackedConfigLoader7:
    case XefuBuildId::kHacked2019:
    case XefuBuildId::kHacked2021a:
    case XefuBuildId::kHacked2021b:
    case XefuBuildId::kHacked2021c:
    case XefuBuildId::kHackedHudPatched:
    case XefuBuildId::kHackedHud1_1:
    case XefuBuildId::kHackedHud2:
    case XefuBuildId::kHackedHud3:
    case XefuBuildId::kHackedHud5:
    case XefuBuildId::kHackedHud6:
    case XefuBuildId::kHackedHud7:
    case XefuBuildId::kHackedHud7b:
    case XefuBuildId::kHackedHudConfigLoader7:
    case XefuBuildId::kHackedHud2019:
    case XefuBuildId::kHackedHud2021a:
    case XefuBuildId::kHackedHud2021b:
    case XefuBuildId::kHackedHud2021c:
      return true;
    default:
      return false;
  }
}

XefuBuildSelectionResult ValidateSelectedBuild(XefuBuildSelection selection,
                                               size_t matching_row_count) {
  XefuBuildSelectionResult result;
  result.matching_row_count = matching_row_count;
  const XefuBuildDescriptor* const descriptor =
      FindKnownXefuBuild(selection.build);
  if (!descriptor) {
    result.diagnostic =
        selection.source == XefuBuildSelectionSource::kManualExactOverride
            ? XefuBuildSelectionDiagnostic::kInvalidManualOverride
            : XefuBuildSelectionDiagnostic::kUnknownEmulatorName;
    return result;
  }
  if (!descriptor->capabilities.preparation_supported) {
    result.diagnostic =
        XefuBuildSelectionDiagnostic::kBuildNotPreparationSupported;
    return result;
  }
  result.selection = selection;
  return result;
}

}  // namespace

std::optional<XefuBuildId> XefuRetailBuildForOfficialEmulatorName(
    std::string_view emulator_name) {
  const auto it = std::find_if(
      kOfficialEmulatorNames.cbegin(), kOfficialEmulatorNames.cend(),
      [emulator_name](const OfficialEmulatorName& entry) {
        return AsciiCaseEqual(entry.name, emulator_name);
      });
  return it == kOfficialEmulatorNames.cend()
             ? std::nullopt
             : std::optional<XefuBuildId>(it->build);
}

std::optional<XefuBuildId> XefuBuildForCommandLineName(std::string_view name) {
  const auto it = std::find_if(kCommandLineBuildNames.cbegin(),
                               kCommandLineBuildNames.cend(),
                               [name](const OfficialEmulatorName& entry) {
                                 return AsciiCaseEqual(entry.name, name);
                               });
  return it == kCommandLineBuildNames.cend()
             ? std::nullopt
             : std::optional<XefuBuildId>(it->build);
}

std::string_view XefuBuildCommandLineName(XefuBuildId build) {
  const auto it = std::find_if(kCommandLineBuildNames.cbegin(),
                               kCommandLineBuildNames.cend(),
                               [build](const OfficialEmulatorName& entry) {
                                 return entry.build == build;
                               });
  return it == kCommandLineBuildNames.cend() ? std::string_view{} : it->name;
}

XefuBuildSelectionResult SelectOfficialXefuBuild(
    uint32_t title_id, std::span<const XefuOfficialSelectorEntry> entries) {
  XefuBuildSelectionResult result;
  bool saw_allowed = false;
  bool saw_disallowed = false;
  for (const XefuOfficialSelectorEntry& entry : entries) {
    if (entry.title_id != title_id) {
      continue;
    }
    ++result.matching_row_count;
    if (entry.live_flag > 1 || entry.allowed_flag > 1) {
      result.diagnostic = XefuBuildSelectionDiagnostic::kInvalidTableEntry;
      return result;
    }
    saw_allowed |= entry.allowed_flag != 0;
    saw_disallowed |= entry.allowed_flag == 0;
  }

  if (result.matching_row_count == 0) {
    result.diagnostic = XefuBuildSelectionDiagnostic::kTitleNotListed;
    return result;
  }
  if (saw_allowed && saw_disallowed) {
    result.diagnostic = XefuBuildSelectionDiagnostic::kAmbiguousTitle;
    return result;
  }
  if (!saw_allowed) {
    result.diagnostic = XefuBuildSelectionDiagnostic::kTitleDisallowed;
    return result;
  }

  std::optional<XefuBuildSelection> selected;
  for (const XefuOfficialSelectorEntry& entry : entries) {
    if (entry.title_id != title_id) {
      continue;
    }
    const std::optional<XefuBuildId> build =
        XefuRetailBuildForOfficialEmulatorName(entry.emulator_name);
    if (!build) {
      result.diagnostic = XefuBuildSelectionDiagnostic::kUnknownEmulatorName;
      return result;
    }
    const XefuBuildSelection candidate{
        *build, entry.live_flag,
        XefuBuildSelectionSource::kOfficialSelectorTable};
    if (selected && (selected->build != candidate.build ||
                     selected->live_enabled != candidate.live_enabled)) {
      result.diagnostic = XefuBuildSelectionDiagnostic::kAmbiguousTitle;
      return result;
    }
    selected = candidate;
  }

  return ValidateSelectedBuild(*selected, result.matching_row_count);
}

XefuBuildSelectionResult SelectManualXefuBuild(XefuBuildId build,
                                               uint32_t live_enabled) {
  if (live_enabled > 1) {
    XefuBuildSelectionResult result;
    result.diagnostic = XefuBuildSelectionDiagnostic::kInvalidManualOverride;
    return result;
  }
  return ValidateSelectedBuild(
      XefuBuildSelection{build, live_enabled,
                         XefuBuildSelectionSource::kManualExactOverride},
      0);
}

std::optional<XefuRestrictionProfile> GetXefuRestrictionProfile(
    XefuBuildId build) {
  const XefuBuildDescriptor* const descriptor = FindKnownXefuBuild(build);
  if (!descriptor) {
    return std::nullopt;
  }

  XefuRestrictionProfile result;
  result.selector_allowlist_bypassed = IsVerifiedModifiedBuild(build);
  result.debug_xbe_keys_supported = IsVerifiedModifiedBuild(build);
  result.runtime_internal_validation_bypassed =
      HasVerifiedInternalValidationBypass(build);
  result.external_config_overlay_supported =
      descriptor->required_external_config_pack.has_value();
  result.companion_required = std::any_of(
      descriptor->requirements.begin(), descriptor->requirements.end(),
      [](const XefuAssetRequirement& requirement) {
        return requirement.role == XefuAssetRole::kTitleModule &&
               requirement.required;
      });
  return result;
}

std::string_view XefuBuildSelectionDiagnosticToString(
    XefuBuildSelectionDiagnostic diagnostic) {
  switch (diagnostic) {
    case XefuBuildSelectionDiagnostic::kNone:
      return "none";
    case XefuBuildSelectionDiagnostic::kTitleNotListed:
      return "the title ID is not present in the selector table";
    case XefuBuildSelectionDiagnostic::kTitleDisallowed:
      return "the selector table does not allow this title ID";
    case XefuBuildSelectionDiagnostic::kAmbiguousTitle:
      return "the selector table has conflicting rows for this title ID";
    case XefuBuildSelectionDiagnostic::kInvalidTableEntry:
      return "the selector table contains an invalid flag value";
    case XefuBuildSelectionDiagnostic::kUnknownEmulatorName:
      return "the selector row names an unknown XeFu runtime";
    case XefuBuildSelectionDiagnostic::kInvalidManualOverride:
      return "the manual XeFu build override is invalid";
    case XefuBuildSelectionDiagnostic::kBuildNotPreparationSupported:
      return "the selected XeFu build is not available for preparation";
  }
  return "unknown XeFu build-selection error";
}

}  // namespace xe::xefu

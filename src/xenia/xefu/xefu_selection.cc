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

namespace {

struct OfficialTitle {
  uint32_t title_id;
  XefuGeneration generation;
};

constexpr OfficialTitle kOfficialTitles[] = {
#include "xenia/xefu/xefu_official_titles.inc"
};

// Titles whose Xbox One / Series release ships its own XeFu.
constexpr OfficialTitle kXboxOneReleases[] = {
    {0x42560002, XefuGeneration::k2021b},  // Chicken Little
    {0x4C410008, XefuGeneration::k2021b},  // Gladius
    {0x4D530002, XefuGeneration::k2019},   // Fuzion Frenzy
    {0x4D530021, XefuGeneration::k2021c},  // Crimson Skies
    {0x5345000B, XefuGeneration::k2021a},  // Gunvalkyrie
};

// Titles the Xbox One-era title tables (xefutitle2019/2021) add that the Xbox
// 360 selector does not list; xefutitle2021 carries them all.
constexpr uint32_t kXboxOneTableTitles[] = {
    0x4541006D,  // Oddworld: Stranger's Wrath
    0x4C410006,  // Secret Weapons Over Normandy
    0x4C410011,  // Star Wars: Battlefront
    0x4D530031,  // Kung Fu Chaos
};

std::optional<XefuGeneration> FindTitle(std::span<const OfficialTitle> titles,
                                        uint32_t title_id) {
  const auto it = std::lower_bound(titles.begin(), titles.end(), title_id,
                                   [](const OfficialTitle& title, uint32_t id) {
                                     return title.title_id < id;
                                   });
  return it != titles.end() && it->title_id == title_id
             ? std::optional<XefuGeneration>(it->generation)
             : std::nullopt;
}

constexpr bool TitlesSorted(std::span<const OfficialTitle> titles) {
  for (size_t i = 1; i < titles.size(); ++i) {
    if (titles[i - 1].title_id >= titles[i].title_id) {
      return false;
    }
  }
  return true;
}
static_assert(TitlesSorted(kOfficialTitles));
static_assert(TitlesSorted(kXboxOneReleases));

}  // namespace

std::optional<XefuBuildId> XefuBuildForGeneration(XefuGeneration generation,
                                                  XefuAssetProvenance pack) {
  using B = XefuBuildId;
  using G = XefuGeneration;
  switch (pack) {
    case XefuAssetProvenance::kRetail:
      switch (generation) {
        case G::kNovember2005:
          return B::kRetailPatched;
        case G::k1_1:
          return B::kRetail1_1;
        case G::k2:
          return B::kRetail2;
        case G::k3:
          return B::kRetail3;
        case G::k5:
          return B::kRetail5;
        case G::k6:
          return B::kRetail6;
        case G::k7:
          return B::kRetail7;
        case G::k7b:
          return B::kRetail7b;
        default:
          return std::nullopt;
      }
    case XefuAssetProvenance::kModified:
      switch (generation) {
        case G::kNovember2005:
          return B::kHackedPatched;
        case G::k1_1:
          return B::kHacked1_1;
        case G::k2:
          return B::kHacked2;
        case G::k3:
          return B::kHacked3;
        case G::k5:
          return B::kHacked5;
        case G::k6:
          return B::kHacked6;
        case G::k7:
          return B::kHacked7;
        case G::k7b:
          return B::kHacked7b;
        case G::k2019:
          return B::kHacked2019;
        case G::k2021a:
          return B::kHacked2021a;
        case G::k2021b:
          return B::kHacked2021b;
        case G::k2021c:
          return B::kHacked2021c;
      }
      break;
    case XefuAssetProvenance::kModifiedWithHud:
      switch (generation) {
        case G::kNovember2005:
          return B::kHackedHudPatched;
        case G::k1_1:
          return B::kHackedHud1_1;
        case G::k2:
          return B::kHackedHud2;
        case G::k3:
          return B::kHackedHud3;
        case G::k5:
          return B::kHackedHud5;
        case G::k6:
          return B::kHackedHud6;
        case G::k7:
          return B::kHackedHud7;
        case G::k7b:
          return B::kHackedHud7b;
        case G::k2019:
          return B::kHackedHud2019;
        case G::k2021a:
          return B::kHackedHud2021a;
        case G::k2021b:
          return B::kHackedHud2021b;
        case G::k2021c:
          return B::kHackedHud2021c;
      }
      break;
  }
  return std::nullopt;
}

XefuAssetProvenance XefuPackForBuild(XefuBuildId build) {
  if (build <= XefuBuildId::kRetail7b) {
    return XefuAssetProvenance::kRetail;
  }
  if (build <= XefuBuildId::kHacked2021c) {
    return XefuAssetProvenance::kModified;
  }
  return XefuAssetProvenance::kModifiedWithHud;
}

std::optional<XefuGeneration> OfficialXefuGenerationForTitle(
    uint32_t title_id) {
  return FindTitle(kOfficialTitles, title_id);
}

XefuAutomaticSelection SelectAutomaticXefuBuild(
    uint32_t title_id, XefuAssetProvenance pack,
    bool community_config_available) {
  const bool modified = pack != XefuAssetProvenance::kRetail;
  const std::optional<XefuGeneration> official =
      OfficialXefuGenerationForTitle(title_id);
  if (modified) {
    if (const std::optional<XefuGeneration> xbox_one =
            FindTitle(kXboxOneReleases, title_id)) {
      return {*XefuBuildForGeneration(*xbox_one, pack),
              XefuAutomaticReason::kXboxOneRelease};
    }
    // The config loader is a release 7 build, so it replaces only a release 7
    // or 7b choice.
    if (community_config_available &&
        (!official || *official == XefuGeneration::k7 ||
         *official == XefuGeneration::k7b)) {
      return {pack == XefuAssetProvenance::kModified
                  ? XefuBuildId::kHackedConfigLoader7
                  : XefuBuildId::kHackedHudConfigLoader7,
              XefuAutomaticReason::kCommunityConfig};
    }
    if (!official && std::find(std::begin(kXboxOneTableTitles),
                               std::end(kXboxOneTableTitles),
                               title_id) != std::end(kXboxOneTableTitles)) {
      return {*XefuBuildForGeneration(XefuGeneration::k2021c, pack),
              XefuAutomaticReason::kXboxOneTitleTable};
    }
  }
  if (official) {
    return {*XefuBuildForGeneration(*official, pack),
            XefuAutomaticReason::kOfficialList};
  }
  return {*XefuBuildForGeneration(XefuGeneration::k7b, pack),
          XefuAutomaticReason::kLatestRelease};
}

std::string_view XefuAutomaticReasonToString(XefuAutomaticReason reason) {
  switch (reason) {
    case XefuAutomaticReason::kXboxOneRelease:
      return "the XeFu shipped with its Xbox One release";
    case XefuAutomaticReason::kCommunityConfig:
      return "its community config, through the config loader";
    case XefuAutomaticReason::kXboxOneTitleTable:
      return "only the Xbox One-era title tables list it";
    case XefuAutomaticReason::kOfficialList:
      return "the official Xbox 360 list";
    case XefuAutomaticReason::kLatestRelease:
      return "not listed, so the latest Xbox 360 release";
  }
  return "unknown";
}

}  // namespace xe::xefu

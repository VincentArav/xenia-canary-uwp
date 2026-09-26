/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_assets.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/crypto/sha256.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/utf8.h"

namespace xe::xefu {
namespace {

constexpr uint8_t HexDigit(char value) {
  if (value >= '0' && value <= '9') {
    return uint8_t(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return uint8_t(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return uint8_t(value - 'A' + 10);
  }
  return 0xFF;
}

template <typename Digest, size_t N>
consteval Digest DigestLiteral(const char (&text)[N]) {
  static_assert(N == Digest{}.size() * 2 + 1,
                "digest literals must contain exactly two hexadecimal digits "
                "per byte");
  Digest digest{};
  for (size_t i = 0; i < digest.size(); ++i) {
    const uint8_t high = HexDigit(text[i * 2]);
    const uint8_t low = HexDigit(text[i * 2 + 1]);
    if (high == 0xFF || low == 0xFF) {
      // A non-constexpr call makes an invalid literal fail constant
      // evaluation without relying on C++ exceptions (disabled in Xenia).
      std::abort();
    }
    digest[i] = uint8_t((high << 4) | low);
  }
  return digest;
}

template <size_t N>
consteval Sha1Digest Sha1Literal(const char (&text)[N]) {
  return DigestLiteral<Sha1Digest>(text);
}

template <size_t N>
consteval Sha256Digest Sha256Literal(const char (&text)[N]) {
  return DigestLiteral<Sha256Digest>(text);
}

constexpr XefuAssetRequirement ExactAsset(XefuAssetRole role,
                                          std::string_view filename,
                                          std::string_view guest_filename,
                                          const Sha1Digest& sha1, uint64_t size,
                                          const Sha256Digest& sha256) {
  return {role, filename, guest_filename, sha1, size, true, sha256};
}

constexpr XefuAssetRequirement kRetailXboxRequirement{ExactAsset(
    XefuAssetRole::kSelector, "xbox.xex", "xbox.xex",
    Sha1Literal("1C577B7495D3D119F5A584B328E837901C537A28"), 135168,
    Sha256Literal(
        "C73C48C7EEC019D77EDE18FA0B7CA8E012086662C2886F4DCBC98AA0DD57DA0C"))};

constexpr std::array<XefuAssetRequirement, 2> kRetailUnpatchedRequirements{{
    {XefuAssetRole::kRuntime, "xefu.xex", "xefu.xex",
     Sha1Literal("A97D65C4860DEDE8A2F58C9C9BBB68270CDD3A64"), 0, true},
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 2> kRetailPatchedRequirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu.xex", "xefu.xex",
               Sha1Literal("87CD5F284C9EC98EA5B9D6D03735F9EA5A05CED6"), 643072,
               Sha256Literal("33EAE143AB6041EB6F250E42C383468CA8CFCA77B22615762"
                             "5235BE5128586CE")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 2> kRetail2Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu2.xex", "xefu2.xex",
               Sha1Literal("39A944C27B8E5F3FB0FCD3A208819177C2B70899"), 647168,
               Sha256Literal("EE7357CF7AF839C0F063312039B65FF53F352EB11165231D5"
                             "CA308CF5070F8F0")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 2> kRetail3Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu3.xex", "xefu3.xex",
               Sha1Literal("1556B2ED2326E5ADC8E75E0E3ADBCC69A5417E9E"), 651264,
               Sha256Literal("57BB4AF0A3D16D7F413838DDDD3F4289C550D28FEEFF0A45A"
                             "18CA2E5FAF02DE1")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 3> kRetail5Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu5.xex", "xefu5.xex",
               Sha1Literal("99D376CE980125AA57E64919C3EB556A1F8AE889"), 638976,
               Sha256Literal("B10BC12E0876E8193C704F0E777EE217AE178078B70159B78"
                             "7F205EDE69E9F29")),
    ExactAsset(XefuAssetRole::kTitleModule, "xefutitle5.xex", "xefutitle5.xex",
               Sha1Literal("B1575B5B8F6245E58283B9F237CEC42E93AF0C4B"), 8192,
               Sha256Literal("41258F25E49C7DAE0470F52707FD250FED3C744D2465CB172"
                             "F033A37C459B87A")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 2> kRetail1_1Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu1_1.xex", "xefu1_1.xex",
               Sha1Literal("7EA7C292DBCA5E85209F0E3A9E14A739A6351859"), 643072,
               Sha256Literal("7A7AE5B73056509F5F72B45D017B8C7585466CF5EC9F0F314"
                             "7CE803AC9663811")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 3> kRetail6Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu6.xex", "xefu6.xex",
               Sha1Literal("D0F6E51DC6E908F8C2944C8B31202D69F26D8E92"), 651264,
               Sha256Literal("EF67F5653EBF988DFDB9E01452B145582F3F11CC4FA621BCD"
                             "EBDDA2370127BB8")),
    ExactAsset(XefuAssetRole::kTitleModule, "xefutitle6.xex", "xefutitle6.xex",
               Sha1Literal("273234CB2F5D87E9D270B35E02ED4319BEECA0D8"), 12288,
               Sha256Literal("83C5B8FE34E0560A7433EE2B40C91063783BCDF579C2197B8"
                             "A794798E7ED07A7")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 3> kRetail7Requirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu7.xex", "xefu7.xex",
               Sha1Literal("D6636CE564F0D424DA646A1FA2B2C89A26B26093"), 663552,
               Sha256Literal("9CADB18AB581C301687C69E2E43A0798957996C62CA7DCC74"
                             "A13F119BD5CA244")),
    ExactAsset(XefuAssetRole::kTitleModule, "xefutitle7.xex", "xefutitle7.xex",
               Sha1Literal("50465A09ACE276CCF2BB152A573AAC2C2C5BC43B"), 12288,
               Sha256Literal("E93FE32F6C786399C3B31DF887EFE3CE8AB9060CFB73936DE"
                             "15FD9839315DB45")),
    kRetailXboxRequirement,
}};

constexpr std::array<XefuAssetRequirement, 3> kRetail7bRequirements{{
    ExactAsset(XefuAssetRole::kRuntime, "xefu7b.xex", "xefu7b.xex",
               Sha1Literal("5650913581E903990FB59988A65DFFB0E5E34A30"), 659456,
               Sha256Literal("7F46FA6AB7DA782ACE09CA91682D245731C99CADC3E161259"
                             "23B1DF44CCD9EAD")),
    ExactAsset(XefuAssetRole::kTitleModule, "xefutitle7b.xex",
               "xefutitle7b.xex",
               Sha1Literal("AAFED130597C3AE815931B8A91038AD5B8B6772F"), 12288,
               Sha256Literal("7E2BCA2182DE593EBAD075C9F1E5469A663227B33F2D616AE"
                             "65AF039824795ED")),
    kRetailXboxRequirement,
}};

constexpr XefuAssetRequirement kHackedXboxRequirement{ExactAsset(
    XefuAssetRole::kSelector, "xbox.xex", "xbox.xex",
    Sha1Literal("C78324F6C1AB3F7997DBC8A0E2B675B439B99C5B"), 198656,
    Sha256Literal(
        "0D2471DFC6574BD04480F78F50DEBCEED2970654C46BCD5D703157C6AA657121"))};
constexpr XefuAssetRequirement kHackedHudXboxRequirement{ExactAsset(
    XefuAssetRole::kSelector, "xbox.xex", "xbox.xex",
    Sha1Literal("4914EF8A17BB587C3D089B83DCF34FF09699785F"), 198656,
    Sha256Literal(
        "FE0652CF3BC0EFE8E66C9C4036BD3CCB943D49BDD4B1C172185C2B618CE04381"))};

constexpr XefuAssetRequirement kHackedTitle5Requirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle5.xex", "xefutitle5.xex",
    Sha1Literal("85580AF0121DB004F9AAEBDC5642D41F90A30F0D"), 8192,
    Sha256Literal(
        "6B0EC0F0031E994335254B28195B411013C03D2CC1993278727D3C51D9DD5E04"))};
constexpr XefuAssetRequirement kHackedTitle6Requirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle6.xex", "xefutitle6.xex",
    Sha1Literal("6B369935740CEC6713D0D4A1BE4FEF225134A749"), 10240,
    Sha256Literal(
        "1A9049F764D9B71E8FA3C19287B952910709E428020BB2DBDF82AEDCE75BF3AA"))};
constexpr XefuAssetRequirement kHackedTitle7Requirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle7.xex", "xefutitle7.xex",
    Sha1Literal("D27422CDA2281160D6C78C57C8C69B662E4EE53D"), 10240,
    Sha256Literal(
        "A201D6C48C433A8F46CCEF3FE5F687A940123238092B5AEB7F7FD954F9BAB468"))};
constexpr XefuAssetRequirement kHackedTitle7bRequirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle7b.xex", "xefutitle7b.xex",
    Sha1Literal("FF527838F4135586E3C52EEEB65F37A250289E36"), 10240,
    Sha256Literal(
        "7193FA16262F60660D92025A831DA399C7E4DFD6BC20595D371174584EA84525"))};
constexpr XefuAssetRequirement kHackedTitle2019Requirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle2019.xex", "xefutitle.xex",
    Sha1Literal("EA11E659203E509A2A92631BE82EB4A5E6A87287"), 10240,
    Sha256Literal(
        "EA1F26916BB77B6D12478C3BCDC230D4CB00F5C197CDC4DEDC0FE1A55032801D"))};
constexpr XefuAssetRequirement kHackedTitle2021Requirement{ExactAsset(
    XefuAssetRole::kTitleModule, "xefutitle2021.xex", "xefutitle.xex",
    Sha1Literal("77E5D7709CB0E7CA5D4CDBA32AFE060A27C9129C"), 10240,
    Sha256Literal(
        "C45DE7520B0091C0DCC0DD44A319251BADC98C4B30A8152B59B8A3A5FDE95720"))};

#define XEFU_STANDALONE_REQUIREMENTS(name, filename, guest_filename, sha1, \
                                     size, sha256, xbox_requirement)       \
  constexpr std::array<XefuAssetRequirement, 2> name {                     \
    {                                                                      \
      ExactAsset(XefuAssetRole::kRuntime, filename, guest_filename,        \
                 Sha1Literal(sha1), size, Sha256Literal(sha256)),          \
          xbox_requirement,                                                \
    }                                                                      \
  }

#define XEFU_PAIRED_REQUIREMENTS(name, filename, guest_filename, sha1, size,  \
                                 sha256, title_requirement, xbox_requirement) \
  constexpr std::array<XefuAssetRequirement, 3> name {                        \
    {                                                                         \
      ExactAsset(XefuAssetRole::kRuntime, filename, guest_filename,           \
                 Sha1Literal(sha1), size, Sha256Literal(sha256)),             \
          title_requirement, xbox_requirement,                                \
    }                                                                         \
  }

XEFU_STANDALONE_REQUIREMENTS(
    kHackedPatchedRequirements, "xefu.xex", "xefu.xex",
    "89D94A83EFEB49D3D78F9D04715A5DB8237D3918", 643072,
    "B2C494ED51187BAFF6EBCF5E47721A49698DF051DD1454BA3283194CFA9A32D3",
    kHackedXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHacked1_1Requirements, "xefu1_1.xex", "xefu1_1.xex",
    "FC9033F023A02E2441631E3BDF2F80082BBF0019", 643072,
    "206202E28ADFAD63AF2FC7E25431678336E6B41F721103E3C827DA69656D2096",
    kHackedXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHacked2Requirements, "xefu2.xex", "xefu2.xex",
    "7BD1EF4BD3DD52912A91178320C6AFCAF5B4D6FC", 647168,
    "57FAD889B703F629D3F72902898B9F09D30B045118133C0C45A9692D9B0C4780",
    kHackedXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHacked3Requirements, "xefu3.xex", "xefu3.xex",
    "7212ACF84F53E57744074AC2395DEDA17C452A52", 651264,
    "6C708E2DC9BF2DED2456C29E9BFA9A48AE671CD36944A09B9433A2F31B9607E3",
    kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked5Requirements, "xefu5.xex", "xefu5.xex",
    "BD078F4A1FE917D005162F10ACEDA2AB8F896E52", 636928,
    "B7B6EAD77AEB19296C6888B5B8DB1C9F20F58D43F7232193F0BBCDC96C5AC76D",
    kHackedTitle5Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked6Requirements, "xefu6.xex", "xefu6.xex",
    "F9334D065422149111D47928941972DAADC1B349", 651264,
    "42F6C1768EDAAC679DF79E934C42B39983E213E9CF2E48109F37366055CFA131",
    kHackedTitle6Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked7Requirements, "xefu7.xex", "xefu7.xex",
    "0734213AC2D0971D1BF65B393755B4C81B14D9FD", 661504,
    "7C822FEEE47AAE99DA1421E3888139E26B9DA0A2A759006265CC519D1F983704",
    kHackedTitle7Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked7bRequirements, "xefu7b.xex", "xefu7b.xex",
    "1E59CF6F218B6F14BD65B749553416656FA276D2", 657408,
    "3712006BBB58B604EA0CDD5E4C4605A6C90AAAF51E7517A3939B95BB55724175",
    kHackedTitle7bRequirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedConfigLoader7Requirements, "config_loader_xefu7.xex", "xefu7.xex",
    "F5D53C8554923D9AE647F9CFA324B625B98B7B3F", 661504,
    "CFE416E6A078667B71BF9EFA206249B7B3838B56981AF984C51FB7C839E08E8F",
    kHackedTitle7Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked2019Requirements, "xefu2019.xex", "xefu.xex",
    "D142159D506D1CDC04C16349CE09FF4204DD7954", 663552,
    "E5617DF8181883A553CD53535AF7B65F365EC5E405E0EFB6A6FA668EDB12BE19",
    kHackedTitle2019Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked2021aRequirements, "xefu2021a.xex", "xefu.xex",
    "0ADE3C11F56B7B085C0D5B357D1945BEF1D07469", 665600,
    "1717A517EE75587F5F489AAF3ADC054E13A46D574BB86E40A645D821EA5E0CA1",
    kHackedTitle2021Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked2021bRequirements, "xefu2021b.xex", "xefu.xex",
    "0200F9125092DFB9414145702485BA408168B5F2", 667648,
    "4C9B2D6EE293484F3379A8F644F94A2C8CD176CADC615F68959B4C2937E861DD",
    kHackedTitle2021Requirement, kHackedXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHacked2021cRequirements, "xefu2021c.xex", "xefu.xex",
    "642DAC8778FA3D5197C67F1501587E2B9B775B1D", 667648,
    "1A53EC6500EF00498E9A216C0A4A6E8DE0462A97404EF227D604083A458187E1",
    kHackedTitle2021Requirement, kHackedXboxRequirement);

XEFU_STANDALONE_REQUIREMENTS(
    kHackedHudPatchedRequirements, "xefu.xex", "xefu.xex",
    "B1BCBCB18EF16F5EBC04AEC31D53A1E6C2255C4F", 643072,
    "303F2AF857E801C156C8D58DBE486E28F1E51F6FFC8886A8190E32F1941F7388",
    kHackedHudXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHackedHud1_1Requirements, "xefu1_1.xex", "xefu1_1.xex",
    "4B08A7C80ABF46A8CA9158F0C6D5A95091B2CFA4", 643072,
    "CF23228AA4F03145FFD6181DE0AEDBC3AFA16B2F84AD7E4FFC6549EB86F2CCC4",
    kHackedHudXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHackedHud2Requirements, "xefu2.xex", "xefu2.xex",
    "8D7855F2FBEAE68878A2789F3FC44F4F37FABD9F", 647168,
    "040F407F28A257AEE2046148DDCD5EBA48CD9D7AEFE9C9C67A94B55E2F9B9C54",
    kHackedHudXboxRequirement);
XEFU_STANDALONE_REQUIREMENTS(
    kHackedHud3Requirements, "xefu3.xex", "xefu3.xex",
    "192D76E1CE3696F4960EAE202895ACC3848D0FF3", 651264,
    "C11946E86F56E9DF8097CCBC042C871D2CEA1BDEE25C0313F0A0D5621E62FD8D",
    kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud5Requirements, "xefu5.xex", "xefu5.xex",
    "F43B450F6D16AFB6176B1FC6947F1684B207732A", 636928,
    "B23371A1A4DF45A93AC74F839B94108E9DA770EB08F236010C5008AFDD345EA1",
    kHackedTitle5Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud6Requirements, "xefu6.xex", "xefu6.xex",
    "FB883A0C14C4464709B00648111ADE06E49ED10C", 651264,
    "53F3D49FA24B982AA41CA9C8993440BF0D3810638495050EE8A69BFC01ACB75C",
    kHackedTitle6Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud7Requirements, "xefu7.xex", "xefu7.xex",
    "65C81C0DABBC8B899107297D124501BA12B08A40", 661504,
    "E05467E9808C86EB4C2B005AE8D144916E09259642EE39807DEA1AB5E8F2BDEB",
    kHackedTitle7Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud7bRequirements, "xefu7b.xex", "xefu7b.xex",
    "AD6A83F26C50CAA8A1DFA818620E37516942B044", 657408,
    "620808298E4140A917AEF08CEFCCE8E99557CED34DE74DF7E4AF040988BCE75B",
    kHackedTitle7bRequirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHudConfigLoader7Requirements, "config_loader_xefu7.xex", "xefu7.xex",
    "1093B5708366675CC1803474CC927E3F326E9484", 661504,
    "C0E8A84AFA265453D01943D5BB0528E51599F2C6516877151903FF48B57BC77C",
    kHackedTitle7Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud2019Requirements, "xefu2019.xex", "xefu.xex",
    "DAC2DE6B89D8C35A5BEF57602D0EB7F664D8D1A5", 663552,
    "01DEC829168F1C1FDFB42A36081B3909394870F66AEAC3711DED23A718E47763",
    kHackedTitle2019Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud2021aRequirements, "xefu2021a.xex", "xefu.xex",
    "CC01DC670CA4F7C75534B347B4F5BB533BC6E9C5", 665600,
    "5078BA32E6AEEEFF65B66E49F6A4BA95BEAF44FD50B24D6D8003115739DB7239",
    kHackedTitle2021Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud2021bRequirements, "xefu2021b.xex", "xefu.xex",
    "1F92B3AB2CCF009A5810C1936EF782E8F48D3A0B", 667648,
    "05296A04A09873C26E3B52ADD4291173B3E19341FCB97E4F6D0A1F9771F5F132",
    kHackedTitle2021Requirement, kHackedHudXboxRequirement);
XEFU_PAIRED_REQUIREMENTS(
    kHackedHud2021cRequirements, "xefu2021c.xex", "xefu.xex",
    "0F32473ABDDB274DDF177E066563DEE6B1DC9A05", 667648,
    "5075F9F84226722FE028FA0E94E519D5F65AA16E86EA75E8C2F8259AA2F4D18E",
    kHackedTitle2021Requirement, kHackedHudXboxRequirement);

#undef XEFU_PAIRED_REQUIREMENTS
#undef XEFU_STANDALONE_REQUIREMENTS

constexpr XefuBuildCapabilities kUnavailableCapabilities{};
constexpr XefuBuildCapabilities kExecutableCapabilities{true, true};

constexpr std::array<XefuBuildDescriptor, 35> kKnownBuilds{{
    {XefuBuildId::kRetailUnpatched, "Retail XeFu (September 2005)",
     XefuAssetProvenance::kRetail, kRetailUnpatchedRequirements,
     kUnavailableCapabilities},
    {XefuBuildId::kRetailPatched, "Retail XeFu (November 2005)",
     XefuAssetProvenance::kRetail, kRetailPatchedRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kRetail2, "Retail XeFu 2", XefuAssetProvenance::kRetail,
     kRetail2Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail3, "Retail XeFu 3", XefuAssetProvenance::kRetail,
     kRetail3Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail5, "Retail XeFu 5", XefuAssetProvenance::kRetail,
     kRetail5Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail1_1, "Retail XeFu 1.1", XefuAssetProvenance::kRetail,
     kRetail1_1Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail6, "Retail XeFu 6", XefuAssetProvenance::kRetail,
     kRetail6Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail7, "Retail XeFu 7", XefuAssetProvenance::kRetail,
     kRetail7Requirements, kExecutableCapabilities},
    {XefuBuildId::kRetail7b, "Retail XeFu 7b", XefuAssetProvenance::kRetail,
     kRetail7bRequirements, kExecutableCapabilities},
    {XefuBuildId::kHackedPatched, "Hacked XeFu (November 2005)",
     XefuAssetProvenance::kModified, kHackedPatchedRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHacked1_1, "Hacked XeFu 1.1", XefuAssetProvenance::kModified,
     kHacked1_1Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked2, "Hacked XeFu 2", XefuAssetProvenance::kModified,
     kHacked2Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked3, "Hacked XeFu 3", XefuAssetProvenance::kModified,
     kHacked3Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked5, "Hacked XeFu 5", XefuAssetProvenance::kModified,
     kHacked5Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked6, "Hacked XeFu 6", XefuAssetProvenance::kModified,
     kHacked6Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked7, "Hacked XeFu 7", XefuAssetProvenance::kModified,
     kHacked7Requirements, kExecutableCapabilities},
    {XefuBuildId::kHacked7b, "Hacked XeFu 7b", XefuAssetProvenance::kModified,
     kHacked7bRequirements, kExecutableCapabilities},
    {XefuBuildId::kHackedConfigLoader7, "Hacked XeFu 7 config loader",
     XefuAssetProvenance::kModified, kHackedConfigLoader7Requirements,
     kExecutableCapabilities, XefuConfigPackFlavor::kModified},
    {XefuBuildId::kHacked2019, "Hacked XeFu 2019",
     XefuAssetProvenance::kModified, kHacked2019Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHacked2021a, "Hacked XeFu 2021a",
     XefuAssetProvenance::kModified, kHacked2021aRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHacked2021b, "Hacked XeFu 2021b",
     XefuAssetProvenance::kModified, kHacked2021bRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHacked2021c, "Hacked XeFu 2021c",
     XefuAssetProvenance::kModified, kHacked2021cRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHudPatched, "Hacked XeFu with HUD (November 2005)",
     XefuAssetProvenance::kModifiedWithHud, kHackedHudPatchedRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud1_1, "Hacked XeFu 1.1 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud1_1Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud2, "Hacked XeFu 2 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud2Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud3, "Hacked XeFu 3 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud3Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud5, "Hacked XeFu 5 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud5Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud6, "Hacked XeFu 6 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud6Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud7, "Hacked XeFu 7 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud7Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud7b, "Hacked XeFu 7b with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud7bRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHudConfigLoader7,
     "Hacked XeFu 7 config loader with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHudConfigLoader7Requirements,
     kExecutableCapabilities, XefuConfigPackFlavor::kModifiedWithHud},
    {XefuBuildId::kHackedHud2019, "Hacked XeFu 2019 with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud2019Requirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud2021a, "Hacked XeFu 2021a with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud2021aRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud2021b, "Hacked XeFu 2021b with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud2021bRequirements,
     kExecutableCapabilities},
    {XefuBuildId::kHackedHud2021c, "Hacked XeFu 2021c with HUD",
     XefuAssetProvenance::kModifiedWithHud, kHackedHud2021cRequirements,
     kExecutableCapabilities},
}};

XefuAssetValidationResult MakeError(XefuAssetError error,
                                    const std::filesystem::path& path) {
  XefuAssetValidationResult result;
  result.error = error;
  result.problem_path = path;
  return result;
}

bool IsManifestFilename(std::string_view filename) {
  return !filename.empty() && filename != "." && filename != ".." &&
         filename.find('/') == std::string_view::npos &&
         filename.find('\\') == std::string_view::npos &&
         filename.find('\0') == std::string_view::npos;
}

bool IsStructurallyValidManifest(const XefuBuildDescriptor& descriptor) {
  if (descriptor.required_external_config_pack) {
    switch (*descriptor.required_external_config_pack) {
      case XefuConfigPackFlavor::kModified:
        if (descriptor.provenance != XefuAssetProvenance::kModified) {
          return false;
        }
        break;
      case XefuConfigPackFlavor::kModifiedWithHud:
        if (descriptor.provenance != XefuAssetProvenance::kModifiedWithHud) {
          return false;
        }
        break;
      case XefuConfigPackFlavor::kNone:
      default:
        return false;
    }
  }

  size_t runtime_count = 0;
  size_t title_module_count = 0;
  size_t selector_count = 0;
  std::vector<std::string> published_filenames;
  published_filenames.reserve(descriptor.requirements.size() * 2);

  for (const XefuAssetRequirement& requirement : descriptor.requirements) {
    if (!IsManifestFilename(requirement.filename) ||
        !IsManifestFilename(requirement.guest_filename)) {
      return false;
    }
    switch (requirement.role) {
      case XefuAssetRole::kRuntime:
        if (!requirement.required) {
          return false;
        }
        ++runtime_count;
        break;
      case XefuAssetRole::kTitleModule:
        ++title_module_count;
        break;
      case XefuAssetRole::kSelector:
        if (!requirement.required) {
          return false;
        }
        ++selector_count;
        break;
      case XefuAssetRole::kResource:
        break;
      default:
        return false;
    }

    const std::string host_filename = utf8::lower_ascii(requirement.filename);
    const std::string guest_filename =
        utf8::lower_ascii(requirement.guest_filename);
    const auto prefix_collides = [](std::string_view left,
                                    std::string_view right) {
      return left.starts_with(right) || right.starts_with(left);
    };
    if (std::any_of(published_filenames.cbegin(), published_filenames.cend(),
                    [&](std::string_view existing) {
                      return prefix_collides(host_filename, existing) ||
                             prefix_collides(guest_filename, existing);
                    }) ||
        (host_filename != guest_filename &&
         prefix_collides(host_filename, guest_filename))) {
      return false;
    }
    published_filenames.push_back(host_filename);
    if (guest_filename != host_filename) {
      published_filenames.push_back(guest_filename);
    }
  }

  // One runtime and one selector, at most one companion, any number of
  // resources.
  return runtime_count == 1 && title_module_count <= 1 && selector_count == 1;
}

}  // namespace

const ValidatedXefuAsset* ValidatedXefuAssetSet::Find(
    XefuAssetRole role) const {
  const auto it = std::find_if(
      assets.cbegin(), assets.cend(),
      [role](const ValidatedXefuAsset& asset) { return asset.role == role; });
  return it == assets.cend() ? nullptr : &*it;
}

bool ComputeFileDigests(const std::filesystem::path& path,
                        XefuFileDigests* digests) {
  if (!digests) {
    return false;
  }

  FILE* file = filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }

  // Hash to EOF, so the size and digests cover exactly the bytes read.
  sha1::SHA1 sha1;
  sha256::SHA256 sha256;
  uint64_t size = 0;
  std::array<uint8_t, 64 * 1024> buffer;
  while (true) {
    const size_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);
    if (bytes_read) {
      if (bytes_read > std::numeric_limits<uint64_t>::max() - size) {
        fclose(file);
        return false;
      }
      size += uint64_t(bytes_read);
      sha1.processBytes(buffer.data(), bytes_read);
      sha256.add(buffer.data(), bytes_read);
    }
    if (bytes_read != buffer.size()) {
      const bool succeeded = feof(file) != 0 && ferror(file) == 0;
      fclose(file);
      if (!succeeded) {
        return false;
      }
      break;
    }
  }

  sha1.finalize(digests->sha1.data());
  sha256.getHash(digests->sha256.data());
  digests->size = size;
  return true;
}

const XefuBuildDescriptor* FindKnownXefuBuild(XefuBuildId id) {
  const auto it = std::find_if(
      kKnownBuilds.cbegin(), kKnownBuilds.cend(),
      [id](const XefuBuildDescriptor& build) { return build.id == id; });
  return it == kKnownBuilds.cend() ? nullptr : &*it;
}

XefuAssetValidationResult ValidateXefuAssets(
    const std::filesystem::path& root, const XefuBuildDescriptor& descriptor) {
  // Reject a malformed descriptor before touching the host filesystem.
  if (!IsStructurallyValidManifest(descriptor)) {
    return MakeError(XefuAssetError::kInvalidManifest, root);
  }

  std::error_code root_status_error;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(root, root_status_error);
  if (root_status_error == std::errc::no_such_file_or_directory ||
      (!root_status_error &&
       root_status.type() == std::filesystem::file_type::not_found)) {
    return MakeError(XefuAssetError::kRootNotFound, root);
  }
  if (root_status_error || std::filesystem::is_symlink(root_status) ||
      filesystem::IsSymlinkOrReparsePoint(root) ||
      !std::filesystem::is_directory(root_status)) {
    return MakeError(XefuAssetError::kRootNotDirectory, root);
  }

  const std::optional<filesystem::FileInfo> root_info =
      filesystem::GetInfo(root);
  if (!root_info) {
    return MakeError(XefuAssetError::kRootNotFound, root);
  }
  if (root_info->type != filesystem::FileInfo::Type::kDirectory) {
    return MakeError(XefuAssetError::kRootNotDirectory, root);
  }

  const std::vector<filesystem::FileInfo> entries = filesystem::ListFiles(root);

  XefuAssetValidationResult result;
  result.assets.build = descriptor.id;
  result.assets.root = root;
  result.assets.assets.reserve(descriptor.requirements.size());

  for (const XefuAssetRequirement& requirement : descriptor.requirements) {
    const std::string wanted_name = utf8::lower_ascii(requirement.filename);
    std::vector<const filesystem::FileInfo*> matches;
    for (const filesystem::FileInfo& entry : entries) {
      const std::string entry_name =
          utf8::lower_ascii(path_to_utf8(entry.name));
      if (entry_name == wanted_name) {
        matches.push_back(&entry);
      }
    }

    const std::filesystem::path requested_path =
        root / to_path(requirement.filename);
    if (matches.empty()) {
      if (requirement.required) {
        return MakeError(XefuAssetError::kMissingFile, requested_path);
      }
      continue;
    }
    if (matches.size() != 1) {
      return MakeError(XefuAssetError::kDuplicateFilename, requested_path);
    }

    const filesystem::FileInfo& entry = *matches.front();
    const std::filesystem::path asset_path = entry.path / entry.name;
    std::error_code asset_status_error;
    const std::filesystem::file_status asset_status =
        std::filesystem::symlink_status(asset_path, asset_status_error);
    if (asset_status_error || std::filesystem::is_symlink(asset_status) ||
        filesystem::IsSymlinkOrReparsePoint(asset_path) ||
        !std::filesystem::is_regular_file(asset_status) ||
        entry.type != filesystem::FileInfo::Type::kFile) {
      return MakeError(XefuAssetError::kNotRegularFile, asset_path);
    }
    if (requirement.size && uint64_t(entry.total_size) != requirement.size) {
      result = MakeError(XefuAssetError::kSizeMismatch, asset_path);
      result.expected_size = requirement.size;
      result.actual_size = entry.total_size;
      return result;
    }

    XefuFileDigests actual_digests;
    if (!ComputeFileDigests(asset_path, &actual_digests)) {
      return MakeError(XefuAssetError::kReadFailed, asset_path);
    }
    if (requirement.size && actual_digests.size != requirement.size) {
      result = MakeError(XefuAssetError::kSizeMismatch, asset_path);
      result.expected_size = requirement.size;
      result.actual_size = actual_digests.size;
      return result;
    }
    if (actual_digests.sha1 != requirement.sha1 ||
        (requirement.sha256 && actual_digests.sha256 != *requirement.sha256)) {
      result = MakeError(XefuAssetError::kHashMismatch, asset_path);
      result.expected_sha1 = requirement.sha1;
      result.actual_sha1 = actual_digests.sha1;
      result.expected_sha256 = requirement.sha256;
      result.actual_sha256 = actual_digests.sha256;
      return result;
    }

    result.assets.assets.push_back(
        {requirement.role, asset_path, std::string(requirement.guest_filename),
         actual_digests.sha1, actual_digests.size, actual_digests.sha256});
  }

  return result;
}

XefuAssetValidationResult ValidateKnownXefuAssets(
    const std::filesystem::path& root, XefuBuildId build) {
  const XefuBuildDescriptor* descriptor = FindKnownXefuBuild(build);
  if (!descriptor) {
    return MakeError(XefuAssetError::kUnsupportedBuild, root);
  }
  return ValidateXefuAssets(root, *descriptor);
}

std::string_view XefuAssetErrorToString(XefuAssetError error) {
  switch (error) {
    case XefuAssetError::kNone:
      return "none";
    case XefuAssetError::kInvalidManifest:
      return "the XeFu asset manifest is structurally invalid";
    case XefuAssetError::kRootNotFound:
      return "compatibility root was not found";
    case XefuAssetError::kRootNotDirectory:
      return "compatibility root is not a directory";
    case XefuAssetError::kMissingFile:
      return "a required XeFu asset is missing";
    case XefuAssetError::kDuplicateFilename:
      return "multiple case-insensitive matches exist for a XeFu asset";
    case XefuAssetError::kNotRegularFile:
      return "a XeFu asset is not a regular file";
    case XefuAssetError::kSizeMismatch:
      return "a XeFu asset has an unexpected size";
    case XefuAssetError::kReadFailed:
      return "a XeFu asset could not be read";
    case XefuAssetError::kHashMismatch:
      return "a XeFu asset hash does not match its selected build";
    case XefuAssetError::kUnsupportedBuild:
      return "the selected XeFu build is not supported";
  }
  return "unknown XeFu asset error";
}

}  // namespace xe::xefu

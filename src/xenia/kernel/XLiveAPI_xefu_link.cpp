/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// clang-format off
#include "xenia/base/platform.h"
#include "third_party/libcurl/include/curl/curl.h"
// clang-format on

#include "xenia/base/logging.h"
#include "xenia/kernel/XLiveAPI.h"

using namespace rapidjson;

namespace xe {
namespace kernel {

// Seconds. The link calls these from its own thread, which title shutdown
// joins.
constexpr uint32_t kXefuLinkHttpTimeout = 5;

bool XLiveAPI::XefuLinkRegister(uint32_t title_id, uint64_t session_id,
                                const std::string& title_name, uint16_t port) {
  SessionObjectJSON session;
  session.SessionID(fmt::format("{:016x}", session_id));
  session.XUID(fmt::format("{:016X}", 0));
  session.Title(title_name);
  session.MediaID("00000000");
  session.Version("xefu-link");
  session.Flags(0);
  session.PublicSlotsCount(4);
  session.PrivateSlotsCount(0);
  session.UserIndex(0);
  session.HostAddress(OnlineIP_str());
  session.MacAddress(GetConsoleMacAddress().to_string());
  session.Port(port);

  std::string body;
  if (!session.Serialize(body)) {
    return false;
  }
  auto response = Post(
      BuildEndpoint(fmt::format("title/{:08X}/sessions", title_id)),
      reinterpret_cast<const uint8_t*>(body.c_str()), 0, kXefuLinkHttpTimeout);
  if (response->StatusCode() != HTTP_STATUS_CODE::HTTP_CREATED) {
    XELOGW("XeFu link: the netplay server did not list this game ({})",
           response->Message());
    return false;
  }
  return true;
}

std::vector<std::pair<std::string, uint16_t>> XLiveAPI::XefuLinkPeers(
    uint32_t title_id, uint64_t own_session_id) {
  std::vector<std::pair<std::string, uint16_t>> peers;
  auto response =
      Get(BuildEndpoint(fmt::format("title/{:08X}/sessions/search", title_id)),
          kXefuLinkHttpTimeout);
  if (response->StatusCode() != HTTP_STATUS_CODE::HTTP_OK) {
    return peers;
  }
  Document doc;
  doc.Parse(response->RawResponse().response);
  if (doc.HasParseError() || !doc.IsArray()) {
    return peers;
  }
  const std::string own_id = fmt::format("{:016x}", own_session_id);
  for (const Value& object : doc.GetArray()) {
    if (!object.IsObject()) {
      continue;
    }
    SessionObjectJSON session;
    if (!session.Deserialize(object.GetObj()) ||
        session.SessionID() == own_id || session.HostAddress().empty() ||
        !session.Port()) {
      continue;
    }
    peers.emplace_back(session.HostAddress(), uint16_t(session.Port()));
  }
  return peers;
}

void XLiveAPI::XefuLinkUnregister(uint32_t title_id, uint64_t session_id) {
  Delete(BuildEndpoint(fmt::format("title/{:08X}/sessions/{:016x}", title_id,
                                   session_id)),
         kXefuLinkHttpTimeout);
}

}  // namespace kernel
}  // namespace xe

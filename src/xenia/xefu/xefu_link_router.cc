/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/xefu/xefu_link_router.h"

#include <algorithm>
#include <charconv>

namespace xe::xefu {
namespace {

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

bool ParseDecimal(std::string_view text, uint32_t max, uint32_t& value) {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() &&
         value <= max;
}

uint64_t ReadMac(std::span<const uint8_t> bytes) {
  uint64_t mac = 0;
  for (size_t i = 0; i < 6; ++i) {
    mac = (mac << 8) | bytes[i];
  }
  return mac;
}

void AddUnique(std::vector<LinkEndpoint>& out, const LinkEndpoint& endpoint) {
  if (std::find(out.begin(), out.end(), endpoint) == out.end()) {
    out.push_back(endpoint);
  }
}

}  // namespace

std::optional<LinkEndpoint> ParseLinkEndpoint(std::string_view text,
                                              uint16_t default_port) {
  text = Trim(text);
  LinkEndpoint endpoint;
  endpoint.port = default_port;
  const size_t colon = text.find(':');
  if (colon != std::string_view::npos) {
    uint32_t port = 0;
    if (!ParseDecimal(text.substr(colon + 1), 0xFFFF, port) || !port) {
      return std::nullopt;
    }
    endpoint.port = uint16_t(port);
    text = text.substr(0, colon);
  }
  for (int part = 0; part < 4; ++part) {
    const size_t dot = part < 3 ? text.find('.') : text.size();
    if (dot == std::string_view::npos) {
      return std::nullopt;
    }
    uint32_t octet = 0;
    if (!ParseDecimal(text.substr(0, dot), 255, octet)) {
      return std::nullopt;
    }
    endpoint.ipv4 = (endpoint.ipv4 << 8) | octet;
    text = part < 3 ? text.substr(dot + 1) : std::string_view();
  }
  return endpoint;
}

std::optional<std::vector<LinkEndpoint>> ParseLinkEndpointList(
    std::string_view text, uint16_t default_port) {
  std::vector<LinkEndpoint> endpoints;
  while (!text.empty()) {
    const size_t separator = text.find_first_of(",;");
    const std::string_view item = Trim(text.substr(0, separator));
    text = separator == std::string_view::npos ? std::string_view()
                                               : text.substr(separator + 1);
    if (item.empty()) {
      continue;
    }
    auto endpoint = ParseLinkEndpoint(item, default_port);
    if (!endpoint) {
      return std::nullopt;
    }
    AddUnique(endpoints, *endpoint);
  }
  return endpoints;
}

bool IsValidLinkFrame(std::span<const uint8_t> frame) {
  return frame.size() >= kLinkMinimumFrameSize &&
         frame.size() <= kLinkMaximumFrameSize;
}

uint64_t LinkDestinationMac(std::span<const uint8_t> frame) {
  return ReadMac(frame.subspan(0, 6));
}

uint64_t LinkSourceMac(std::span<const uint8_t> frame) {
  return ReadMac(frame.subspan(6, 6));
}

bool IsGroupMac(uint64_t mac) { return (mac >> 40) & 1; }

LinkRouter::LinkRouter(std::vector<LinkEndpoint> static_peers,
                       std::vector<LinkEndpoint> broadcast_targets)
    : static_peers_(std::move(static_peers)),
      broadcast_targets_(std::move(broadcast_targets)) {}

void LinkRouter::Flood(std::vector<LinkEndpoint>& out, uint64_t now_ms) const {
  for (const LinkEndpoint& endpoint : static_peers_) {
    AddUnique(out, endpoint);
  }
  for (const LinkEndpoint& endpoint : broadcast_targets_) {
    AddUnique(out, endpoint);
  }
  for (const LinkEndpoint& endpoint : discovered_peers_) {
    AddUnique(out, endpoint);
  }
  for (const auto& [mac, learned] : learned_) {
    if (now_ms - learned.last_seen_ms < kPeerLifetimeMs) {
      AddUnique(out, learned.endpoint);
    }
  }
}

std::vector<LinkEndpoint> LinkRouter::RouteOutgoing(
    std::span<const uint8_t> frame, uint64_t now_ms) {
  std::vector<LinkEndpoint> out;
  if (!IsValidLinkFrame(frame)) {
    return out;
  }
  own_macs_.insert(LinkSourceMac(frame));
  const uint64_t destination = LinkDestinationMac(frame);
  if (!IsGroupMac(destination)) {
    if (auto endpoint = LearnedEndpoint(destination, now_ms)) {
      out.push_back(*endpoint);
      return out;
    }
  }
  Flood(out, now_ms);
  return out;
}

LinkIncoming LinkRouter::AcceptIncoming(std::span<const uint8_t> frame,
                                        const LinkEndpoint& from,
                                        uint64_t now_ms) {
  if (!IsValidLinkFrame(frame)) {
    return LinkIncoming::kMalformed;
  }
  const uint64_t source = LinkSourceMac(frame);
  if (own_macs_.count(source)) {
    return LinkIncoming::kOwnFrame;
  }
  if (!IsGroupMac(source)) {
    learned_[source] = {from, now_ms};
  }
  return LinkIncoming::kDeliver;
}

std::optional<LinkEndpoint> LinkRouter::LearnedEndpoint(uint64_t mac,
                                                        uint64_t now_ms) const {
  auto it = learned_.find(mac);
  if (it == learned_.end() ||
      now_ms - it->second.last_seen_ms >= kPeerLifetimeMs) {
    return std::nullopt;
  }
  return it->second.endpoint;
}

}  // namespace xe::xefu

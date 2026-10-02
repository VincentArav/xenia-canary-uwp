/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_XEFU_XEFU_LINK_ROUTER_H_
#define XENIA_XEFU_XEFU_LINK_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xe::xefu {

// System link carries each Ethernet frame XeFu hands to xam, unchanged, as one
// UDP datagram, as xemu's UDP network backend does; peers may be XeFu or xemu.
constexpr size_t kLinkMinimumFrameSize = 14;
// A full frame with an 802.1Q tag, without the frame check sequence.
constexpr size_t kLinkMaximumFrameSize = 1518;
constexpr uint16_t kLinkDefaultPort = 9368;

struct LinkEndpoint {
  // Host byte order.
  uint32_t ipv4 = 0;
  uint16_t port = 0;

  bool operator==(const LinkEndpoint&) const = default;
};

struct LinkEndpointHash {
  size_t operator()(const LinkEndpoint& endpoint) const {
    return (size_t(endpoint.ipv4) << 16) ^ endpoint.port;
  }
};

// Parses "a.b.c.d:port" or "a.b.c.d" (the default port). Host names are not
// resolved here.
std::optional<LinkEndpoint> ParseLinkEndpoint(std::string_view text,
                                              uint16_t default_port);
// Parses a comma- or semicolon-separated list, skipping empty items; nullopt
// if any item is malformed.
std::optional<std::vector<LinkEndpoint>> ParseLinkEndpointList(
    std::string_view text, uint16_t default_port);

bool IsValidLinkFrame(std::span<const uint8_t> frame);
// The 48-bit MAC in the low bits.
uint64_t LinkDestinationMac(std::span<const uint8_t> frame);
uint64_t LinkSourceMac(std::span<const uint8_t> frame);
// Broadcast and multicast destinations have the group bit set.
bool IsGroupMac(uint64_t mac);

enum class LinkIncoming {
  kDeliver,
  // One of this instance's own frames, for example a broadcast looped back.
  kOwnFrame,
  kMalformed,
};

// Chooses where frames go and which arriving frames the guest sees. Peers are
// learned from the source MAC of arriving frames; a frame for an unknown or
// group destination floods to every static peer, broadcast target and learned
// peer. Not thread-safe.
class LinkRouter {
 public:
  static constexpr uint64_t kPeerLifetimeMs = 5 * 60 * 1000;
  // Any sender can add itself; the table stays small and bounded.
  static constexpr size_t kMaxLearnedPeers = 256;

  LinkRouter(std::vector<LinkEndpoint> static_peers,
             std::vector<LinkEndpoint> broadcast_targets);

  // Endpoints for a guest frame, or none if the frame is malformed. Records
  // the frame's source MAC as this instance's.
  std::vector<LinkEndpoint> RouteOutgoing(std::span<const uint8_t> frame,
                                          uint64_t now_ms);

  // Classifies an arriving frame and learns its sender.
  LinkIncoming AcceptIncoming(std::span<const uint8_t> frame,
                              const LinkEndpoint& from, uint64_t now_ms);

  // Peers found through the netplay server, replacing the previous set.
  void SetDiscoveredPeers(std::vector<LinkEndpoint> peers) {
    discovered_peers_ = std::move(peers);
  }

  std::optional<LinkEndpoint> LearnedEndpoint(uint64_t mac,
                                              uint64_t now_ms) const;

 private:
  struct Learned {
    LinkEndpoint endpoint;
    uint64_t last_seen_ms;
  };

  void Flood(std::vector<LinkEndpoint>& out, uint64_t now_ms) const;

  std::vector<LinkEndpoint> static_peers_;
  std::vector<LinkEndpoint> broadcast_targets_;
  std::vector<LinkEndpoint> discovered_peers_;
  std::unordered_map<uint64_t, Learned> learned_;
  std::unordered_set<uint64_t> own_macs_;
};

}  // namespace xe::xefu

#endif  // XENIA_XEFU_XEFU_LINK_ROUTER_H_

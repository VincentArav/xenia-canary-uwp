/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xefu_link.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// clang-format off
#include "xenia/base/platform.h"
// clang-format on

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/upnp.h"
#include "xenia/kernel/user_mode_kernel_runtime.h"
#include "xenia/kernel/util/network_adapter_manager.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xefu/xefu_link_router.h"
#include "xenia/xefu/xefu_session.h"

#ifdef XE_PLATFORM_WIN32
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

DEFINE_bool(xefu_system_link, true,
            "Carry original Xbox system link traffic from XeFu games to other "
            "instances (or xemu) as UDP. Only XeFu sessions are affected.",
            "XeFu");
DEFINE_int32(xefu_link_port, xe::xefu::kLinkDefaultPort,
             "UDP port for XeFu system link. A busy port moves to the next of "
             "four, so several instances can run on one computer.",
             "XeFu");
DEFINE_string(xefu_link_peers, "",
              "Other computers to exchange XeFu system link traffic with, for "
              "play over a VPN or the internet: comma-separated ip or ip:port.",
              "XeFu");
DEFINE_bool(xefu_link_online, true,
            "Find other players of the same original Xbox game through the "
            "netplay server (network_mode 2). The port must be reachable: "
            "enable upnp or forward it.",
            "XeFu");
DEFINE_bool(xefu_link_broadcast, true,
            "Find XeFu system link peers on the local network by broadcast.",
            "XeFu");

DECLARE_int32(network_mode);

namespace xe::kernel::xefu_link {
namespace {

using xe::xefu::LinkEndpoint;
using xe::xefu::LinkIncoming;
using xe::xefu::LinkRouter;

#ifdef XE_PLATFORM_WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void CloseSocket(SocketHandle s) { close(s); }
#endif

constexpr uint16_t kPortSpan = 4;
constexpr size_t kQueueLimit = 256;
constexpr uint32_t kSlotCount = 16;
constexpr uint32_t kSlotSize = 1536;
constexpr uint32_t kSlotsOffset = 64;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

sockaddr_in ToSockaddr(const LinkEndpoint& endpoint) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(endpoint.ipv4);
  address.sin_port = htons(endpoint.port);
  return address;
}

class Link {
 public:
  explicit Link(KernelState* kernel_state) : kernel_state_(kernel_state) {}
  ~Link() { Stop(); }

  bool Start();
  void Stop();
  void ReleaseGuestMemory();

  void SetCallbacks(uint32_t filter, uint32_t receive, uint32_t context);
  bool Transmit(std::span<const uint8_t> frame);
  void Deliver(cpu::ppc::PPCContext* ppc_context);

 private:
  void ReceiveLoop();
  void RendezvousLoop();
  void QueueDelivery();

  KernelState* kernel_state_;
  SocketHandle socket_ = kInvalidSocket;
  uint16_t port_ = 0;
  std::thread receive_thread_;
  std::thread rendezvous_thread_;
  std::atomic<bool> stopping_{false};
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;

  std::mutex router_mutex_;
  std::unique_ptr<LinkRouter> router_;

  std::mutex queue_mutex_;
  std::deque<std::vector<uint8_t>> queue_;
  uint64_t dropped_ = 0;
  std::atomic<bool> dpc_queued_{false};
  std::atomic<bool> logged_transmit_{false};
  std::atomic<bool> logged_delivery_{false};

  // Guest state, written by the guest's registration.
  std::atomic<uint32_t> filter_{0};
  std::atomic<uint32_t> receive_{0};
  std::atomic<uint32_t> context_{0};
  // An XDPC at +0 and kSlotCount frame buffers from kSlotsOffset.
  std::atomic<uint32_t> guest_block_{0};
  uint32_t next_slot_ = 0;
};

std::mutex g_link_mutex;
std::shared_ptr<Link> g_link;
cpu::Function* g_deliver_function = nullptr;

std::shared_ptr<Link> CurrentLink() {
  std::lock_guard<std::mutex> lock(g_link_mutex);
  return g_link;
}

void DeliverThunk(cpu::ppc::PPCContext* ppc_context, void*, void*) {
  if (auto link = CurrentLink()) {
    link->Deliver(ppc_context);
  }
}

bool Link::Start() {
#ifdef XE_PLATFORM_WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data)) {
    XELOGE("XeFu link: WSAStartup failed");
    return false;
  }
#endif
  socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == kInvalidSocket) {
    XELOGE("XeFu link: could not create a UDP socket");
    return false;
  }
  int enable = 1;
  setsockopt(socket_, SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char*>(&enable), sizeof(enable));
  int buffer_size = 1 << 20;
  setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
             reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
#ifdef XE_PLATFORM_WIN32
  DWORD timeout_ms = 20;
  setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  timeval timeout{0, 20000};
  setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

  const uint16_t base_port = uint16_t(cvars::xefu_link_port);
  for (uint16_t offset = 0; offset < kPortSpan && !port_; ++offset) {
    sockaddr_in address = ToSockaddr({0, uint16_t(base_port + offset)});
    if (!bind(socket_, reinterpret_cast<sockaddr*>(&address),
              sizeof(address))) {
      port_ = uint16_t(base_port + offset);
    }
  }
  if (!port_) {
    XELOGE("XeFu link: UDP ports {}-{} are all in use", base_port,
           base_port + kPortSpan - 1);
    CloseSocket(socket_);
    socket_ = kInvalidSocket;
    return false;
  }

  auto peers =
      xe::xefu::ParseLinkEndpointList(cvars::xefu_link_peers, base_port);
  if (!peers) {
    XELOGE("XeFu link: ignoring malformed xefu_link_peers \"{}\"",
           cvars::xefu_link_peers);
    peers.emplace();
  }
  // Broadcast reaches other computers on the network; loopback reaches other
  // instances on this one, which bind the neighbouring ports.
  std::vector<LinkEndpoint> broadcast_targets;
  if (cvars::xefu_link_broadcast) {
    for (uint16_t offset = 0; offset < kPortSpan; ++offset) {
      const uint16_t port = uint16_t(base_port + offset);
      broadcast_targets.push_back({0xFFFFFFFF, port});
      if (port != port_) {
        broadcast_targets.push_back({0x7F000001, port});
      }
    }
  }
  router_ = std::make_unique<LinkRouter>(*peers, broadcast_targets);

  receive_thread_ = std::thread([this] { ReceiveLoop(); });
  if (cvars::xefu_link_online &&
      cvars::network_mode == NETWORK_MODE::XBOXLIVE) {
    rendezvous_thread_ = std::thread([this] { RendezvousLoop(); });
  }
  XELOGI(
      "XeFu link: system link on UDP port {}, {} static peer(s), "
      "broadcast {}",
      port_, peers->size(), cvars::xefu_link_broadcast ? "on" : "off");
  return true;
}

void Link::Stop() {
  {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    stopping_.store(true);
  }
  stop_cv_.notify_all();
  if (rendezvous_thread_.joinable()) {
    rendezvous_thread_.join();
  }
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }
  if (socket_ != kInvalidSocket) {
    CloseSocket(socket_);
    socket_ = kInvalidSocket;
#ifdef XE_PLATFORM_WIN32
    WSACleanup();
#endif
  }
}

void Link::ReleaseGuestMemory() {
  if (const uint32_t block = guest_block_.exchange(0)) {
    kernel_state_->memory()->SystemHeapFree(block);
  }
}

void Link::SetCallbacks(uint32_t filter, uint32_t receive, uint32_t context) {
  if (!receive) {
    receive_.store(0);
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
    return;
  }
  if (!guest_block_.load()) {
    const uint32_t block = kernel_state_->memory()->SystemHeapAlloc(
        kSlotsOffset + kSlotCount * kSlotSize);
    if (!block) {
      XELOGE("XeFu link: no guest memory for received frames");
      return;
    }
    kernel_state_->memory()->TranslateVirtual<XDPC*>(block)->Initialize(
        g_deliver_function->address(), 0);
    guest_block_.store(block);
  }
  // The receive callback goes last: the receive thread checks it first.
  filter_.store(filter);
  context_.store(context);
  receive_.store(receive);
  QueueDelivery();
}

bool Link::Transmit(std::span<const uint8_t> frame) {
  std::vector<LinkEndpoint> targets;
  {
    std::lock_guard<std::mutex> lock(router_mutex_);
    targets = router_->RouteOutgoing(frame, NowMs());
  }
  if (!targets.empty() && !logged_transmit_.exchange(true)) {
    XELOGI("XeFu link: the game sent its first frame ({} bytes)", frame.size());
  }
  for (const LinkEndpoint& target : targets) {
    sockaddr_in address = ToSockaddr(target);
    sendto(socket_, reinterpret_cast<const char*>(frame.data()),
           int(frame.size()), 0, reinterpret_cast<sockaddr*>(&address),
           sizeof(address));
  }
  return !targets.empty();
}

void Link::ReceiveLoop() {
  std::vector<uint8_t> buffer(2048);
  while (!stopping_.load()) {
    sockaddr_in from{};
#ifdef XE_PLATFORM_WIN32
    int from_size = sizeof(from);
#else
    socklen_t from_size = sizeof(from);
#endif
    const int size = int(recvfrom(
        socket_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
        reinterpret_cast<sockaddr*>(&from), &from_size));
    if (size > 0) {
      std::span<const uint8_t> frame(buffer.data(), size_t(size));
      const LinkEndpoint sender{ntohl(from.sin_addr.s_addr),
                                ntohs(from.sin_port)};
      LinkIncoming verdict;
      {
        std::lock_guard<std::mutex> lock(router_mutex_);
        verdict = router_->AcceptIncoming(frame, sender, NowMs());
      }
      if (verdict == LinkIncoming::kDeliver && receive_.load()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= kQueueLimit) {
          queue_.pop_front();
          if (!(dropped_++ % 1000)) {
            XELOGW("XeFu link: receive queue full, dropping frames");
          }
        }
        queue_.emplace_back(frame.begin(), frame.end());
      }
    }
    // Also retries a delivery the runtime could not queue.
    QueueDelivery();
  }
}

// Lists this instance on the netplay server under the original Xbox title's
// own ID and polls for the others, so system link works over the internet.
void Link::RendezvousLoop() {
  Emulator* emulator = kernel_state_->emulator();
  const xe::xefu::MountedXefuSession* session =
      emulator->mounted_xefu_session();
  XLiveAPI* api = emulator->GetXboxLiveAPI();
  if (!session || !session->nested_title_id || !api) {
    return;
  }
  const uint32_t title_id = session->nested_title_id;
  std::random_device random;
  const uint64_t session_id =
      0xAE00000000000000ull |
      ((uint64_t(random()) << 32 | random()) & 0x00FFFFFFFFFFFFFFull);

  UPnP* upnp = emulator->GetUPnP();
  if (upnp && upnp->IsActive()) {
    // In host byte order, as netplay's own bind path passes it.
    upnp->AddPortAsync(
        emulator->GetNetworkAdapterManager()->GetSelectedAdapterLocalIPString(),
        port_, "UDP");
  }

  bool registered = false;
  size_t last_count = SIZE_MAX;
  std::unique_lock<std::mutex> lock(stop_mutex_);
  while (!stopping_.load()) {
    lock.unlock();
    if (api->IsConnectedToServer()) {
      if (!registered) {
        registered = api->XefuLinkRegister(title_id, session_id,
                                           emulator->title_name(), port_);
        if (registered) {
          XELOGI("XeFu link: listed {:08X} on the netplay server", title_id);
        }
      }
      std::vector<LinkEndpoint> peers;
      for (const auto& [host, port] :
           api->XefuLinkPeers(title_id, session_id)) {
        if (auto endpoint = xe::xefu::ParseLinkEndpoint(host, port)) {
          peers.push_back(*endpoint);
        }
      }
      if (peers.size() != last_count) {
        XELOGI("XeFu link: {} other player(s) of this game online",
               peers.size());
        last_count = peers.size();
      }
      std::lock_guard<std::mutex> router_lock(router_mutex_);
      router_->SetDiscoveredPeers(std::move(peers));
    }
    lock.lock();
    stop_cv_.wait_for(lock, std::chrono::seconds(10),
                      [this] { return stopping_.load(); });
  }
  lock.unlock();
  if (registered) {
    api->XefuLinkUnregister(title_id, session_id);
  }
  if (upnp && upnp->IsActive()) {
    upnp->RemovePortAsync(upnp->GetMappedBindPort(port_), "UDP");
  }
}

void Link::QueueDelivery() {
  const uint32_t block = guest_block_.load();
  if (!receive_.load() || !block) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
      return;
    }
  }
  if (dpc_queued_.exchange(true)) {
    return;
  }
  // The receive callback runs as a DPC on processor 0, as the guest expects.
  UserModeKernelRuntime* runtime = kernel_state_->user_mode_kernel_runtime();
  if (!runtime || !runtime->QueueProcessorDpc(0, block, 0, 0)) {
    dpc_queued_.store(false);
  }
}

void Link::Deliver(cpu::ppc::PPCContext* ppc_context) {
  // Frames arriving from here on queue another DPC.
  dpc_queued_.store(false);
  const uint32_t filter = filter_.load();
  const uint32_t receive = receive_.load();
  const uint32_t context = context_.load();
  const uint32_t block = guest_block_.load();
  if (!receive || !block) {
    return;
  }
  cpu::Processor* processor = kernel_state_->processor();
  auto* memory = kernel_state_->memory();
  for (uint32_t n = 0; n < kSlotCount; ++n) {
    std::vector<uint8_t> frame;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (queue_.empty()) {
        break;
      }
      frame = std::move(queue_.front());
      queue_.pop_front();
    }
    const uint32_t slot =
        block + kSlotsOffset + (next_slot_++ % kSlotCount) * kSlotSize;
    std::memcpy(memory->TranslateVirtual(slot), frame.data(), frame.size());
    if (!logged_delivery_.exchange(true)) {
      XELOGI("XeFu link: delivering a peer's frames to the game");
    }
    uint64_t args[] = {context, slot, frame.size()};
    if (filter &&
        !uint32_t(processor->Execute(ppc_context->thread_state, filter, args,
                                     xe::countof(args)))) {
      continue;
    }
    processor->Execute(ppc_context->thread_state, receive, args,
                       xe::countof(args));
  }
  QueueDelivery();
}

}  // namespace

bool IsEnabled(KernelState* kernel_state) {
  return cvars::xefu_system_link && kernel_state && kernel_state->emulator() &&
         kernel_state->emulator()->mounted_xefu_session();
}

void SetCallbacks(KernelState* kernel_state, uint32_t filter, uint32_t receive,
                  uint32_t context) {
  if (!IsEnabled(kernel_state)) {
    return;
  }
  std::shared_ptr<Link> link = CurrentLink();
  if (!link) {
    if (!receive) {
      return;
    }
    if (!g_deliver_function) {
      g_deliver_function = kernel_state->processor()->DefineBuiltin(
          "XefuLinkDeliver", DeliverThunk, nullptr, nullptr);
      if (!g_deliver_function) {
        return;
      }
    }
    link = std::make_shared<Link>(kernel_state);
    if (!link->Start()) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_link_mutex);
    g_link = link;
  }
  link->SetCallbacks(filter, receive, context);
}

bool Transmit(std::span<const uint8_t> frame) {
  std::shared_ptr<Link> link = CurrentLink();
  return link && link->Transmit(frame);
}

void Shutdown() {
  std::shared_ptr<Link> link;
  {
    std::lock_guard<std::mutex> lock(g_link_mutex);
    link = std::move(g_link);
  }
  if (link) {
    link->Stop();
    link->ReleaseGuestMemory();
  }
}

}  // namespace xe::kernel::xefu_link

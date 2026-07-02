#include "feed/feed.hpp"

#include "itch/itch.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace nsq::feed {

namespace {

template <typename T>
std::vector<std::uint8_t> encode_bytes(const T& m) {
  std::vector<std::uint8_t> buf(T::kSize);
  itch::encode(m, buf.data());
  return buf;
}

bool is_multicast(const in_addr& a) {
  const auto first = ntohl(a.s_addr) >> 24;
  return first >= 224 && first <= 239;
}

}  // namespace

std::vector<std::uint8_t> to_itch(const engine::MarketEvent& ev,
                                  std::uint16_t stock_locate) {
  const itch::Header hdr{stock_locate, 0, ev.ts};
  return std::visit(
      [&](const auto& e) -> std::vector<std::uint8_t> {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, AddedEvent>) {
          itch::AddOrder m{};
          static_cast<itch::Header&>(m) = hdr;
          m.order_ref = e.id;
          m.side = e.side;
          m.shares = e.qty;
          m.stock = ev.symbol;
          m.price = e.price;
          return encode_bytes(m);
        } else if constexpr (std::is_same_v<T, ExecutedEvent>) {
          itch::OrderExecuted m{};
          static_cast<itch::Header&>(m) = hdr;
          m.order_ref = e.resting_id;
          m.executed_shares = e.qty;
          m.match_number = e.match_id;
          return encode_bytes(m);
        } else if constexpr (std::is_same_v<T, CanceledEvent>) {
          if (e.removed) {
            itch::OrderDelete m{};
            static_cast<itch::Header&>(m) = hdr;
            m.order_ref = e.id;
            return encode_bytes(m);
          }
          itch::OrderCancel m{};
          static_cast<itch::Header&>(m) = hdr;
          m.order_ref = e.id;
          m.canceled_shares = e.canceled_qty;
          return encode_bytes(m);
        } else {
          static_assert(std::is_same_v<T, ReplacedEvent>);
          itch::OrderReplace m{};
          static_cast<itch::Header&>(m) = hdr;
          m.original_ref = e.old_id;
          m.new_ref = e.new_id;
          m.shares = e.qty;
          m.price = e.price;
          return encode_bytes(m);
        }
      },
      ev.ev);
}

FeedPublisher::FeedPublisher(MpscQueue<engine::MarketEvent>& in,
                             FeedConfig cfg)
    : in_(in),
      cfg_(std::move(cfg)),
      packetizer_(mold::make_session(cfg_.session), cfg_.mtu) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) throw std::runtime_error("feed socket() failed");

  in_addr dest{};
  if (::inet_pton(AF_INET, cfg_.dest_ip.c_str(), &dest) != 1)
    throw std::runtime_error("bad feed dest ip");
  if (is_multicast(dest)) {
    const std::uint8_t loop = 1;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    in_addr ifaddr{};
    if (::inet_pton(AF_INET, cfg_.mcast_if_ip.c_str(), &ifaddr) == 1)
      ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr);
  }
}

FeedPublisher::~FeedPublisher() {
  stop();
  if (fd_ >= 0) ::close(fd_);
}

void FeedPublisher::start() {
  thread_ = std::thread([this] { run(); });
}

void FeedPublisher::stop() {
  if (!thread_.joinable()) return;
  in_.stop();
  thread_.join();
}

void FeedPublisher::run() {
  using namespace std::chrono_literals;
  for (;;) {
    const auto evs = in_.wait_drain_for(500ms);
    if (evs.empty()) {
      if (in_.stopped()) break;
      send_datagram(packetizer_.heartbeat());
      continue;
    }
    publish(evs);
  }
  publish(in_.drain());  // anything raced in during shutdown
  send_datagram(packetizer_.end_of_session());
}

void FeedPublisher::publish(const std::vector<engine::MarketEvent>& evs) {
  for (const auto& ev : evs) {
    const auto bytes = to_itch(ev, locate_for(ev.symbol));
    packetizer_.add_message(bytes.data(), bytes.size());
  }
  packetizer_.flush();
  send_ready();
}

void FeedPublisher::send_ready() {
  for (const auto& pkt : packetizer_.take_packets()) send_datagram(pkt);
}

void FeedPublisher::send_datagram(const std::vector<std::uint8_t>& bytes) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.dest_port);
  ::inet_pton(AF_INET, cfg_.dest_ip.c_str(), &addr.sin_addr);
  ::sendto(fd_, bytes.data(), bytes.size(), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  ++packets_sent_;
}

std::uint16_t FeedPublisher::locate_for(const Symbol& symbol) {
  const auto it = locates_.find(symbol);
  if (it != locates_.end()) return it->second;
  const auto locate = static_cast<std::uint16_t>(locates_.size() + 1);
  locates_.emplace(symbol, locate);
  return locate;
}

}  // namespace nsq::feed

// ITCH market data feed publisher: consumes engine MarketEvents, encodes
// ITCH 5.0 messages, packetizes via MoldUDP64, and sends UDP datagrams
// (multicast group or unicast address — same send path). Sends a Mold
// heartbeat when idle and an end-of-session packet on shutdown.
#pragma once

#include "common/queue.hpp"
#include "engine/engine.hpp"
#include "mold/mold.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace nsq::feed {

using ::nsq::MpscQueue;

// Translate one market event into ITCH message bytes.
std::vector<std::uint8_t> to_itch(const engine::MarketEvent& ev,
                                  std::uint16_t stock_locate);

struct FeedConfig {
  std::string dest_ip = "239.192.0.1";  // multicast by default
  std::uint16_t dest_port = 26000;
  std::string session = "NSQSIM";
  std::size_t mtu = 1400;
  // Interface for outgoing multicast; loopback lets a same-host listener
  // receive the feed.
  std::string mcast_if_ip = "127.0.0.1";
};

class FeedPublisher {
 public:
  FeedPublisher(MpscQueue<engine::MarketEvent>& in, FeedConfig cfg);
  ~FeedPublisher();

  void start();
  // Drains outstanding events, flushes, sends end-of-session, joins.
  void stop();

  std::uint64_t packets_sent() const { return packets_sent_; }

 private:
  void run();
  void publish(const std::vector<engine::MarketEvent>& evs);
  void send_ready();
  void send_datagram(const std::vector<std::uint8_t>& bytes);
  std::uint16_t locate_for(const Symbol& symbol);

  MpscQueue<engine::MarketEvent>& in_;
  FeedConfig cfg_;
  int fd_ = -1;
  mold::Packetizer packetizer_;
  std::map<Symbol, std::uint16_t> locates_;
  std::atomic<std::uint64_t> packets_sent_{0};
  std::thread thread_;
};

}  // namespace nsq::feed

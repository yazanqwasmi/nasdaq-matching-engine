// MoldUDP64 downstream packetizer/depacketizer.
//
// Downstream packet: session(10, space-padded) + sequence number(8) +
// message count(2), followed by `count` blocks of length(2) + payload.
// The sequence number is the seq of the FIRST message in the packet; a
// heartbeat carries the next expected seq with count 0; end-of-session
// uses count 0xFFFF. Retransmission requests are out of scope (gap
// detection only).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace nsq::mold {

using Session = std::array<char, 10>;

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kEndOfSessionCount = 0xFFFF;

inline Session make_session(std::string_view sv) {
  Session s;
  s.fill(' ');
  for (std::size_t i = 0; i < sv.size() && i < s.size(); ++i) s[i] = sv[i];
  return s;
}

class Packetizer {
 public:
  explicit Packetizer(Session session, std::size_t mtu = 1400)
      : session_(session), mtu_(mtu) {}

  // Appends one message to the current packet, starting a new packet first
  // if this message would push the current one past the MTU.
  void add_message(const std::uint8_t* data, std::size_t n);

  // Closes the current packet (if any) so take_packets() returns it.
  void flush();

  std::vector<std::vector<std::uint8_t>> take_packets();

  std::vector<std::uint8_t> heartbeat() const;
  std::vector<std::uint8_t> end_of_session() const;

  std::uint64_t next_seq() const { return next_seq_; }

 private:
  void begin_packet();

  Session session_;
  std::size_t mtu_;
  std::uint64_t next_seq_ = 1;
  std::vector<std::uint8_t> current_;
  std::uint16_t current_count_ = 0;
  std::vector<std::vector<std::uint8_t>> ready_;
};

struct DownstreamPacket {
  Session session;
  std::uint64_t seq;
  bool end_of_session;
  std::vector<std::vector<std::uint8_t>> messages;
};

std::optional<DownstreamPacket> parse(const std::uint8_t* p, std::size_t n);

// Tracks expected sequence numbers on the receive side.
// on_packet returns true when a gap was detected (and resyncs past it).
class GapDetector {
 public:
  bool on_packet(std::uint64_t seq, std::size_t message_count);
  std::uint64_t expected() const { return expected_; }
  bool had_gap() const { return had_gap_; }

 private:
  std::uint64_t expected_ = 1;
  bool had_gap_ = false;
};

}  // namespace nsq::mold

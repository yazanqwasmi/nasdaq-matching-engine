#include "mold/mold.hpp"

#include "common/endian.hpp"

#include <algorithm>
#include <utility>

namespace nsq::mold {

namespace {
std::vector<std::uint8_t> make_header(const Session& session,
                                      std::uint64_t seq,
                                      std::uint16_t count) {
  std::vector<std::uint8_t> p(kHeaderSize);
  for (std::size_t i = 0; i < session.size(); ++i)
    p[i] = static_cast<std::uint8_t>(session[i]);
  put_be64(p.data() + 10, seq);
  put_be16(p.data() + 18, count);
  return p;
}
}  // namespace

void Packetizer::begin_packet() {
  current_ = make_header(session_, next_seq_, 0);
  current_count_ = 0;
}

void Packetizer::add_message(const std::uint8_t* data, std::size_t n) {
  const std::size_t block = 2 + n;
  if (!current_.empty() && current_.size() + block > mtu_) flush();
  if (current_.empty()) begin_packet();

  const std::size_t at = current_.size();
  current_.resize(at + block);
  put_be16(current_.data() + at, static_cast<std::uint16_t>(n));
  if (n > 0) std::copy(data, data + n, current_.data() + at + 2);
  ++current_count_;
  ++next_seq_;
}

void Packetizer::flush() {
  if (current_.empty()) return;
  put_be16(current_.data() + 18, current_count_);
  ready_.push_back(std::move(current_));
  current_.clear();
  current_count_ = 0;
}

std::vector<std::vector<std::uint8_t>> Packetizer::take_packets() {
  return std::exchange(ready_, {});
}

std::vector<std::uint8_t> Packetizer::heartbeat() const {
  return make_header(session_, next_seq_, 0);
}

std::vector<std::uint8_t> Packetizer::end_of_session() const {
  return make_header(session_, next_seq_, kEndOfSessionCount);
}

std::optional<DownstreamPacket> parse(const std::uint8_t* p, std::size_t n) {
  if (n < kHeaderSize) return std::nullopt;
  DownstreamPacket pkt;
  for (std::size_t i = 0; i < pkt.session.size(); ++i)
    pkt.session[i] = static_cast<char>(p[i]);
  pkt.seq = get_be64(p + 10);
  const std::uint16_t count = get_be16(p + 18);
  pkt.end_of_session = (count == kEndOfSessionCount);

  std::size_t at = kHeaderSize;
  if (!pkt.end_of_session) {
    for (std::uint16_t i = 0; i < count; ++i) {
      if (at + 2 > n) return std::nullopt;
      const std::uint16_t len = get_be16(p + at);
      at += 2;
      if (at + len > n) return std::nullopt;
      pkt.messages.emplace_back(p + at, p + at + len);
      at += len;
    }
  }
  if (at != n) return std::nullopt;  // trailing garbage
  return pkt;
}

bool GapDetector::on_packet(std::uint64_t seq, std::size_t message_count) {
  const bool gap = seq > expected_;
  if (gap) had_gap_ = true;
  // Heartbeats (count 0) advertise the next seq without consuming any.
  const std::uint64_t after = seq + message_count;
  if (message_count > 0 && after > expected_) expected_ = after;
  if (gap && message_count == 0 && seq > expected_) expected_ = seq;
  return gap;
}

}  // namespace nsq::mold

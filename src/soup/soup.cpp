#include "soup/soup.hpp"

#include "common/endian.hpp"

#include <algorithm>
#include <cstring>

namespace nsq::soup {

namespace {

// Alphanumeric fields are left-justified space-padded; the sequence number
// field is right-justified space-filled ASCII digits, per the Soup spec.
void append_alpha(std::vector<std::uint8_t>& out, std::string_view s,
                  std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    out.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ');
}

void append_seq(std::vector<std::uint8_t>& out, std::uint64_t seq,
                std::size_t width) {
  std::string digits = std::to_string(seq);
  for (std::size_t i = digits.size(); i < width; ++i) out.push_back(' ');
  for (char c : digits) out.push_back(static_cast<std::uint8_t>(c));
}

std::string trim_field(const std::uint8_t* p, std::size_t n) {
  std::size_t begin = 0, end = n;
  while (begin < end && p[begin] == ' ') ++begin;
  while (end > begin && p[end - 1] == ' ') --end;
  return {reinterpret_cast<const char*>(p) + begin, end - begin};
}

}  // namespace

void append_packet(std::vector<std::uint8_t>& out, char type,
                   const std::uint8_t* payload, std::size_t n) {
  const auto len = static_cast<std::uint16_t>(n + 1);
  const std::size_t at = out.size();
  out.resize(at + 2);
  put_be16(out.data() + at, len);
  out.push_back(static_cast<std::uint8_t>(type));
  if (n > 0) out.insert(out.end(), payload, payload + n);
}

void append_login_request(std::vector<std::uint8_t>& out,
                          std::string_view username, std::string_view password,
                          std::string_view requested_session,
                          std::uint64_t requested_seq) {
  std::vector<std::uint8_t> payload;
  payload.reserve(46);
  append_alpha(payload, username, 6);
  append_alpha(payload, password, 10);
  append_alpha(payload, requested_session, 10);
  append_seq(payload, requested_seq, 20);
  append_packet(out, kLoginRequest, payload.data(), payload.size());
}

std::optional<LoginRequest> decode_login_request(const std::uint8_t* p,
                                                 std::size_t n) {
  if (n != 46) return std::nullopt;
  LoginRequest req;
  req.username = trim_field(p, 6);
  req.password = trim_field(p + 6, 10);
  req.requested_session = trim_field(p + 16, 10);
  const std::string seq = trim_field(p + 26, 20);
  req.requested_seq = 0;
  for (char c : seq) {
    if (c < '0' || c > '9') return std::nullopt;
    req.requested_seq = req.requested_seq * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return req;
}

void FrameParser::push(const std::uint8_t* data, std::size_t n) {
  buf_.insert(buf_.end(), data, data + n);
}

std::optional<FrameParser::Packet> FrameParser::next() {
  const std::size_t avail = buf_.size() - consumed_;
  if (avail < 2) return std::nullopt;
  const std::uint16_t len = get_be16(buf_.data() + consumed_);
  if (len < 1 || avail < 2u + len) return std::nullopt;

  Packet pkt;
  pkt.type = static_cast<char>(buf_[consumed_ + 2]);
  pkt.payload.assign(buf_.begin() + static_cast<std::ptrdiff_t>(consumed_ + 3),
                     buf_.begin() + static_cast<std::ptrdiff_t>(consumed_ + 2 + len));
  consumed_ += 2u + len;

  // Compact once consumed bytes dominate the buffer.
  if (consumed_ > 4096 && consumed_ * 2 > buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
  }
  return pkt;
}

void ServerSession::queue(char type, const std::uint8_t* payload,
                          std::size_t n) {
  append_packet(out_, type, payload, n);
}

void ServerSession::on_bytes(const std::uint8_t* data, std::size_t n,
                             std::uint64_t now_ns) {
  if (state_ == State::Closed) return;
  last_recv_ns_ = now_ns;
  parser_.push(data, n);
  while (auto pkt = parser_.next()) {
    if (state_ == State::AwaitingLogin) {
      if (pkt->type == kLoginRequest) {
        if (auto req = decode_login_request(pkt->payload.data(),
                                            pkt->payload.size())) {
          listener_.on_login_request(*req);
        } else {
          reject_login('A');
          return;
        }
      }
      // Anything else before login is ignored.
    } else if (state_ == State::LoggedIn) {
      switch (pkt->type) {
        case kUnsequenced:
          listener_.on_unsequenced(pkt->payload.data(), pkt->payload.size());
          break;
        case kLogoutRequest:
          state_ = State::Closed;
          listener_.on_logout();
          return;
        case kClientHeartbeat:
        case kDebug:
        default:
          break;  // heartbeats already refreshed last_recv_ns_
      }
    }
    if (state_ == State::Closed) return;
  }
}

void ServerSession::tick(std::uint64_t now_ns) {
  if (state_ == State::Closed) return;
  if (now_ns - last_recv_ns_ >= kSessionTimeoutNs) {
    state_ = State::Closed;
    listener_.on_timeout();
    return;
  }
  if (state_ == State::LoggedIn &&
      now_ns - last_send_ns_ >= kHeartbeatIntervalNs) {
    queue(kServerHeartbeat, nullptr, 0);
    last_send_ns_ = now_ns;
  }
}

void ServerSession::accept_login(std::string_view session,
                                 std::uint64_t next_seq) {
  std::vector<std::uint8_t> payload;
  payload.reserve(30);
  {
    // session: 10 alpha; sequence: 20-digit right-justified.
    for (std::size_t i = 0; i < 10; ++i)
      payload.push_back(i < session.size()
                            ? static_cast<std::uint8_t>(session[i])
                            : ' ');
    std::string digits = std::to_string(next_seq);
    for (std::size_t i = digits.size(); i < 20; ++i) payload.push_back(' ');
    for (char c : digits) payload.push_back(static_cast<std::uint8_t>(c));
  }
  queue(kLoginAccepted, payload.data(), payload.size());
  state_ = State::LoggedIn;
}

void ServerSession::reject_login(char reason) {
  const auto r = static_cast<std::uint8_t>(reason);
  queue(kLoginRejected, &r, 1);
  state_ = State::Closed;
}

void ServerSession::send_sequenced(const std::uint8_t* payload,
                                   std::size_t n) {
  queue(kSequenced, payload, n);
}

void ServerSession::send_end_of_session() { queue(kEndOfSession, nullptr, 0); }

std::vector<std::uint8_t> ServerSession::take_output() {
  return std::exchange(out_, {});
}

}  // namespace nsq::soup

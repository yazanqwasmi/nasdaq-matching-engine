// SoupBinTCP framing and server-side session state machine tests.
// The session is a pure state machine: bytes in, bytes out, explicit time.
#include "soup/soup.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nsq::soup {
namespace {

constexpr std::uint64_t kSec = 1'000'000'000ULL;

std::vector<std::uint8_t> login_request_bytes(std::string_view user,
                                              std::string_view pass,
                                              std::string_view session,
                                              std::uint64_t seq) {
  std::vector<std::uint8_t> out;
  append_login_request(out, user, pass, session, seq);
  return out;
}

TEST(SoupFraming, PacketLayout) {
  std::vector<std::uint8_t> out;
  const std::uint8_t payload[] = {0xAA, 0xBB};
  append_packet(out, kSequenced, payload, 2);
  // length field counts type byte + payload = 3
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out[0], 0x00);
  EXPECT_EQ(out[1], 0x03);
  EXPECT_EQ(out[2], 'S');
  EXPECT_EQ(out[3], 0xAA);
  EXPECT_EQ(out[4], 0xBB);
}

TEST(SoupFraming, ParserHandlesPartialAndCoalescedPackets) {
  FrameParser parser;
  std::vector<std::uint8_t> stream;
  append_packet(stream, kClientHeartbeat, nullptr, 0);
  append_packet(stream, kUnsequenced, reinterpret_cast<const std::uint8_t*>("hi"), 2);

  // Feed one byte at a time: packets appear only when complete.
  std::vector<std::pair<char, std::string>> got;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    parser.push(&stream[i], 1);
    while (auto p = parser.next()) {
      got.emplace_back(p->type,
                       std::string(p->payload.begin(), p->payload.end()));
    }
  }
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].first, kClientHeartbeat);
  EXPECT_EQ(got[1].first, kUnsequenced);
  EXPECT_EQ(got[1].second, "hi");

  // Two packets in one push are both extracted.
  parser.push(stream.data(), stream.size());
  int n = 0;
  while (parser.next()) ++n;
  EXPECT_EQ(n, 2);
}

TEST(SoupLogin, RequestRoundTrip) {
  const auto bytes = login_request_bytes("USER1", "PASS", "SESSA", 7);
  // 2 (length) + 1 (type) + 6 + 10 + 10 + 20
  ASSERT_EQ(bytes.size(), 49u);
  EXPECT_EQ(bytes[2], 'L');

  FrameParser parser;
  parser.push(bytes.data(), bytes.size());
  const auto p = parser.next();
  ASSERT_TRUE(p.has_value());
  const auto req = decode_login_request(p->payload.data(), p->payload.size());
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->username, "USER1");
  EXPECT_EQ(req->password, "PASS");
  EXPECT_EQ(req->requested_session, "SESSA");
  EXPECT_EQ(req->requested_seq, 7u);
}

struct RecordingListener : ServerSession::Listener {
  std::vector<LoginRequest> logins;
  std::vector<std::string> unsequenced;
  int logouts = 0;
  int timeouts = 0;

  void on_login_request(const LoginRequest& r) override { logins.push_back(r); }
  void on_unsequenced(const std::uint8_t* data, std::size_t n) override {
    unsequenced.emplace_back(reinterpret_cast<const char*>(data), n);
  }
  void on_logout() override { ++logouts; }
  void on_timeout() override { ++timeouts; }
};

class SoupSessionTest : public ::testing::Test {
 protected:
  RecordingListener listener;
  ServerSession session{listener, /*now_ns=*/0};

  void deliver_login(std::uint64_t now = 0) {
    const auto bytes = login_request_bytes("USER1", "PASS", "", 1);
    session.on_bytes(bytes.data(), bytes.size(), now);
  }
};

TEST_F(SoupSessionTest, LoginAcceptFlow) {
  EXPECT_EQ(session.state(), ServerSession::State::AwaitingLogin);
  deliver_login();
  ASSERT_EQ(listener.logins.size(), 1u);
  EXPECT_EQ(listener.logins[0].username, "USER1");

  session.accept_login("SESS01", 1);
  EXPECT_EQ(session.state(), ServerSession::State::LoggedIn);

  const auto out = session.take_output();
  // Login Accepted: 2 + 1 + 10 + 20
  ASSERT_EQ(out.size(), 33u);
  EXPECT_EQ(out[2], 'A');
}

TEST_F(SoupSessionTest, LoginRejectClosesSession) {
  deliver_login();
  session.reject_login('A');  // not authorized
  EXPECT_EQ(session.state(), ServerSession::State::Closed);
  const auto out = session.take_output();
  ASSERT_EQ(out.size(), 4u);  // 2 + 1 + 1
  EXPECT_EQ(out[2], 'J');
  EXPECT_EQ(out[3], 'A');
}

TEST_F(SoupSessionTest, DataBeforeLoginIsIgnored) {
  const std::uint8_t junk[] = {'x'};
  std::vector<std::uint8_t> pkt;
  append_packet(pkt, kUnsequenced, junk, 1);
  session.on_bytes(pkt.data(), pkt.size(), 0);
  EXPECT_TRUE(listener.unsequenced.empty());
}

TEST_F(SoupSessionTest, UnsequencedPayloadDelivered) {
  deliver_login();
  session.accept_login("S", 1);
  std::vector<std::uint8_t> pkt;
  append_packet(pkt, kUnsequenced,
                reinterpret_cast<const std::uint8_t*>("ouch!"), 5);
  session.on_bytes(pkt.data(), pkt.size(), 0);
  ASSERT_EQ(listener.unsequenced.size(), 1u);
  EXPECT_EQ(listener.unsequenced[0], "ouch!");
}

TEST_F(SoupSessionTest, SendSequencedFramesPayload) {
  deliver_login();
  session.accept_login("S", 1);
  session.take_output();
  session.send_sequenced(reinterpret_cast<const std::uint8_t*>("DATA"), 4);
  const auto out = session.take_output();
  ASSERT_EQ(out.size(), 7u);
  EXPECT_EQ(out[2], 'S');
  EXPECT_EQ(out[3], 'D');
}

TEST_F(SoupSessionTest, ServerHeartbeatAfterOneSecondIdle) {
  deliver_login();
  session.accept_login("S", 1);
  session.take_output();

  session.tick(kSec / 2);
  EXPECT_TRUE(session.take_output().empty());

  session.tick(kSec + kSec / 10);
  const auto out = session.take_output();
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 'H');
}

TEST_F(SoupSessionTest, ClientSilenceTimesOutAfter15Seconds) {
  deliver_login();
  session.accept_login("S", 1);

  session.tick(14 * kSec);
  EXPECT_EQ(listener.timeouts, 0);
  EXPECT_EQ(session.state(), ServerSession::State::LoggedIn);

  session.tick(16 * kSec);
  EXPECT_EQ(listener.timeouts, 1);
  EXPECT_EQ(session.state(), ServerSession::State::Closed);
}

TEST_F(SoupSessionTest, ClientHeartbeatKeepsSessionAlive) {
  deliver_login();
  session.accept_login("S", 1);

  std::vector<std::uint8_t> hb;
  append_packet(hb, kClientHeartbeat, nullptr, 0);
  session.on_bytes(hb.data(), hb.size(), 14 * kSec);

  session.tick(16 * kSec);  // only 2s since last client traffic
  EXPECT_EQ(listener.timeouts, 0);
  EXPECT_EQ(session.state(), ServerSession::State::LoggedIn);
}

TEST_F(SoupSessionTest, LogoutRequestClosesSession) {
  deliver_login();
  session.accept_login("S", 1);
  std::vector<std::uint8_t> pkt;
  append_packet(pkt, kLogoutRequest, nullptr, 0);
  session.on_bytes(pkt.data(), pkt.size(), 0);
  EXPECT_EQ(listener.logouts, 1);
  EXPECT_EQ(session.state(), ServerSession::State::Closed);
}

TEST_F(SoupSessionTest, EndOfSessionPacket) {
  deliver_login();
  session.accept_login("S", 1);
  session.take_output();
  session.send_end_of_session();
  const auto out = session.take_output();
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2], 'Z');
}

}  // namespace
}  // namespace nsq::soup

// End-to-end TCP slice test: a raw client connects to the gateway, logs in
// over SoupBinTCP, enters two crossing orders via OUCH, and receives
// Accepted + Executed responses over the wire.
#include "engine/engine.hpp"
#include "gateway/gateway.hpp"
#include "soup/soup.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace nsq {
namespace {

class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd_, 0);
    timeval tv{2, 0};  // 2s receive timeout so a hung test fails fast
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr),
              0)
        << strerror(errno);
  }
  ~TestClient() {
    if (fd_ >= 0) ::close(fd_);
  }

  void send_bytes(const std::vector<std::uint8_t>& bytes) {
    ASSERT_EQ(::send(fd_, bytes.data(), bytes.size(), 0),
              static_cast<ssize_t>(bytes.size()));
  }

  void send_unsequenced(const std::uint8_t* payload, std::size_t n) {
    std::vector<std::uint8_t> out;
    soup::append_packet(out, soup::kUnsequenced, payload, n);
    send_bytes(out);
  }

  // Blocks until the next soup packet arrives (skipping server heartbeats).
  soup::FrameParser::Packet next_packet() {
    for (;;) {
      if (auto p = parser_.next()) {
        if (p->type == soup::kServerHeartbeat) continue;
        return *p;
      }
      std::uint8_t buf[4096];
      const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
      if (n <= 0) {
        ADD_FAILURE() << "recv failed or timed out: " << strerror(errno);
        return {'?', {}};
      }
      parser_.push(buf, static_cast<std::size_t>(n));
    }
  }

 private:
  int fd_ = -1;
  soup::FrameParser parser_;
};

std::vector<std::uint8_t> enter_order_bytes(std::string_view token, Side side,
                                            Price price, Qty shares) {
  ouch::EnterOrder m{};
  m.token = ouch::make_token(token);
  m.side = side;
  m.shares = shares;
  m.stock = make_symbol("AAPL");
  m.price = price;
  m.tif = 99999;
  m.firm = {'T', 'E', 'S', 'T'};
  m.display = 'Y';
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.cross_type = 'N';
  m.customer_type = 'R';
  std::vector<std::uint8_t> buf(ouch::kEnterOrderSize);
  ouch::encode(m, buf.data());
  return buf;
}

TEST(GatewaySlice, LoginEnterCrossExecuteOverTcp) {
  engine::MpscQueue<engine::Command> to_engine;
  engine::MpscQueue<engine::ClientResponse> to_gateway;
  engine::MpscQueue<engine::MarketEvent> to_feed;

  engine::Engine engine{to_engine, to_gateway, to_feed};
  engine.start();
  gateway::Gateway gw{0, to_engine, to_gateway};
  gw.start();
  ASSERT_NE(gw.port(), 0);

  TestClient client(gw.port());

  // Login.
  std::vector<std::uint8_t> login;
  soup::append_login_request(login, "USER1", "PASS", "", 1);
  client.send_bytes(login);
  const auto accept = client.next_packet();
  ASSERT_EQ(accept.type, soup::kLoginAccepted);

  // Two crossing orders from the same client.
  const auto sell = enter_order_bytes("SELL1", Side::Sell, make_price(100, 0), 100);
  const auto buy = enter_order_bytes("BUY1", Side::Buy, make_price(100, 0), 100);
  client.send_unsequenced(sell.data(), sell.size());
  client.send_unsequenced(buy.data(), buy.size());

  // Expect sequenced OUCH: Accepted, Accepted, Executed, Executed.
  std::vector<char> types;
  std::vector<soup::FrameParser::Packet> packets;
  for (int i = 0; i < 4; ++i) {
    auto p = client.next_packet();
    ASSERT_EQ(p.type, soup::kSequenced);
    ASSERT_FALSE(p.payload.empty());
    types.push_back(static_cast<char>(p.payload[0]));
    packets.push_back(std::move(p));
  }
  EXPECT_EQ((std::vector<char>{'A', 'A', 'E', 'E'}), types);

  const auto exec = ouch::decode_executed(packets[2].payload.data(),
                                          packets[2].payload.size());
  ASSERT_TRUE(exec.has_value());
  EXPECT_EQ(ouch::token_view(exec->token), "SELL1");
  EXPECT_EQ(exec->executed_shares, 100u);
  EXPECT_EQ(exec->execution_price, make_price(100, 0));

  // The feed queue saw the resting add + the execution.
  const auto evs = to_feed.drain();
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_NE(std::get_if<AddedEvent>(&evs[0].ev), nullptr);
  EXPECT_NE(std::get_if<ExecutedEvent>(&evs[1].ev), nullptr);

  gw.stop();
  engine.stop();
}

TEST(GatewaySlice, OversizeOrderRejectedAtGateway) {
  engine::MpscQueue<engine::Command> to_engine;
  engine::MpscQueue<engine::ClientResponse> to_gateway;
  engine::MpscQueue<engine::MarketEvent> to_feed;

  engine::Engine engine{to_engine, to_gateway, to_feed};
  engine.start();
  gateway::Gateway gw{0, to_engine, to_gateway};
  gw.start();

  TestClient client(gw.port());
  std::vector<std::uint8_t> login;
  soup::append_login_request(login, "USER1", "PASS", "", 1);
  client.send_bytes(login);
  ASSERT_EQ(client.next_packet().type, soup::kLoginAccepted);

  // 2M shares exceeds the gateway's per-order risk cap.
  const auto big =
      enter_order_bytes("BIG1", Side::Buy, make_price(100, 0), 2'000'000);
  client.send_unsequenced(big.data(), big.size());

  const auto p = client.next_packet();
  ASSERT_EQ(p.type, soup::kSequenced);
  const auto rej = ouch::decode_rejected(p.payload.data(), p.payload.size());
  ASSERT_TRUE(rej.has_value());
  EXPECT_EQ(ouch::token_view(rej->token), "BIG1");

  gw.stop();
  engine.stop();
}

}  // namespace
}  // namespace nsq

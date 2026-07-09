// OuchClient tests against a real gateway + engine over TCP.
#include "client/ouch_client.hpp"
#include "engine/engine.hpp"
#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace nsq::client {
namespace {

struct Recorder : OuchClient::Handler {
  std::vector<ouch::Accepted> accepted;
  std::vector<ouch::Executed> executed;
  std::vector<ouch::Canceled> canceled;
  std::vector<ouch::Replaced> replaced;
  std::vector<ouch::Rejected> rejected;

  void on_accepted(const ouch::Accepted& m) override { accepted.push_back(m); }
  void on_executed(const ouch::Executed& m) override { executed.push_back(m); }
  void on_canceled(const ouch::Canceled& m) override { canceled.push_back(m); }
  void on_replaced(const ouch::Replaced& m) override { replaced.push_back(m); }
  void on_rejected(const ouch::Rejected& m) override { rejected.push_back(m); }
};

class OuchClientTest : public ::testing::Test {
 protected:
  engine::CommandChannel to_engine;
  engine::MpscQueue<engine::ClientResponse> to_gateway;
  engine::MpscQueue<engine::MarketEvent> to_feed;
  engine::Engine engine{to_engine, to_gateway, to_feed};
  gateway::Gateway gw{0, to_engine, to_gateway};

  void SetUp() override {
    engine.start();
    gw.start();
  }
  void TearDown() override {
    gw.stop();
    engine.stop();
  }

  // Polls until pred() or 2s elapse.
  template <typename Pred>
  bool poll_until(OuchClient& c, Pred pred) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pred()) {
      c.poll();
      if (std::chrono::steady_clock::now() > deadline) return false;
    }
    return true;
  }
};

TEST_F(OuchClientTest, ConnectEnterExecuteCancelReplace) {
  Recorder rec;
  OuchClient c{rec};
  ASSERT_TRUE(c.connect("127.0.0.1", gw.port(), "AGENT1", "PW"));

  c.enter("S1", Side::Sell, "AAPL", make_price(100, 0), 200);
  c.enter("B1", Side::Buy, "AAPL", make_price(100, 0), 100);
  ASSERT_TRUE(poll_until(c, [&] { return rec.executed.size() >= 2; }));
  EXPECT_EQ(rec.accepted.size(), 2u);
  EXPECT_EQ(rec.executed[0].executed_shares, 100u);

  // 100 left on S1: replace it, then cancel the replacement.
  c.replace("S1", "S2", make_price(101, 0), 100);
  ASSERT_TRUE(poll_until(c, [&] { return !rec.replaced.empty(); }));
  EXPECT_EQ(ouch::token_view(rec.replaced[0].replacement_token), "S2");

  c.cancel("S2");
  ASSERT_TRUE(poll_until(c, [&] { return !rec.canceled.empty(); }));
  EXPECT_EQ(rec.canceled[0].decrement_shares, 100u);

  c.close();
}

TEST_F(OuchClientTest, RejectionDelivered) {
  Recorder rec;
  OuchClient c{rec};
  ASSERT_TRUE(c.connect("127.0.0.1", gw.port(), "AGENT2", "PW"));
  c.enter("BIG", Side::Buy, "AAPL", make_price(100, 0), 2'000'000);
  ASSERT_TRUE(poll_until(c, [&] { return !rec.rejected.empty(); }));
  EXPECT_EQ(ouch::token_view(rec.rejected[0].token), "BIG");
  c.close();
}

}  // namespace
}  // namespace nsq::client

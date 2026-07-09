// Engine tests: commands in, OUCH-shaped responses + market events out.
// The engine is exercised synchronously via process(); the thread wrapper
// adds no logic beyond queue pumping (covered by the gateway integration
// test and the queue test below).
#include "engine/engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace nsq::engine {
namespace {

Command enter(std::uint64_t client, std::string_view token, Side side,
              std::string_view stock, Price price, Qty shares) {
  ouch::EnterOrder m{};
  m.token = ouch::make_token(token);
  m.side = side;
  m.shares = shares;
  m.stock = make_symbol(stock);
  m.price = price;
  m.tif = 99999;
  m.display = 'Y';
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.cross_type = 'N';
  m.customer_type = 'R';
  return Command{client, m};
}

// EnterOrder with explicit time-in-force / min-quantity for the TIF tests.
Command enter_tif(std::uint64_t client, std::string_view token, Side side,
                  std::string_view stock, Price price, Qty shares,
                  std::uint32_t tif, Qty min_qty = 0) {
  ouch::EnterOrder m{};
  m.token = ouch::make_token(token);
  m.side = side;
  m.shares = shares;
  m.stock = make_symbol(stock);
  m.price = price;
  m.tif = tif;
  m.min_qty = min_qty;
  m.display = 'Y';
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.cross_type = 'N';
  m.customer_type = 'R';
  return Command{client, m};
}

class EngineTest : public ::testing::Test {
 protected:
  CommandChannel in;
  MpscQueue<ClientResponse> to_gateway;
  MpscQueue<MarketEvent> to_feed;
  Engine engine{in, to_gateway, to_feed};

  std::vector<ClientResponse> responses() { return to_gateway.drain(); }
  std::vector<MarketEvent> events() { return to_feed.drain(); }
};

TEST_F(EngineTest, RestingOrderAcceptedAndPublished) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(100, 0), 500));

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 1u);
  EXPECT_EQ(rs[0].client_id, 1u);
  const auto* acc = std::get_if<ouch::Accepted>(&rs[0].msg);
  ASSERT_NE(acc, nullptr);
  EXPECT_EQ(ouch::token_view(acc->token), "T1");
  EXPECT_EQ(acc->order_state, 'L');
  EXPECT_GT(acc->order_ref, 0u);

  const auto evs = events();
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(symbol_view(evs[0].symbol), "AAPL");
  EXPECT_NE(std::get_if<AddedEvent>(&evs[0].ev), nullptr);
}

TEST_F(EngineTest, CrossingOrdersFillBothClients) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 300));
  engine.process(enter(2, "B1", Side::Buy, "AAPL", make_price(100, 0), 300));

  const auto rs = responses();
  // Accepted(S1), Accepted(B1), Executed(seller), Executed(buyer)
  ASSERT_EQ(rs.size(), 4u);
  const auto* acc2 = std::get_if<ouch::Accepted>(&rs[1].msg);
  ASSERT_NE(acc2, nullptr);

  const auto* ex_seller = std::get_if<ouch::Executed>(&rs[2].msg);
  const auto* ex_buyer = std::get_if<ouch::Executed>(&rs[3].msg);
  ASSERT_NE(ex_seller, nullptr);
  ASSERT_NE(ex_buyer, nullptr);
  EXPECT_EQ(rs[2].client_id, 1u);
  EXPECT_EQ(ouch::token_view(ex_seller->token), "S1");
  EXPECT_EQ(rs[3].client_id, 2u);
  EXPECT_EQ(ouch::token_view(ex_buyer->token), "B1");
  EXPECT_EQ(ex_seller->executed_shares, 300u);
  EXPECT_EQ(ex_seller->execution_price, make_price(100, 0));
  EXPECT_EQ(ex_seller->match_number, ex_buyer->match_number);
  EXPECT_EQ(ex_seller->liquidity_flag, 'A');
  EXPECT_EQ(ex_buyer->liquidity_flag, 'R');

  // Market events: Added(sell) then Executed. The fully-filled aggressor
  // never rests so no second Added.
  const auto evs = events();
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_NE(std::get_if<AddedEvent>(&evs[0].ev), nullptr);
  EXPECT_NE(std::get_if<ExecutedEvent>(&evs[1].ev), nullptr);
}

TEST_F(EngineTest, BooksAreIndependentPerSymbol) {
  engine.process(enter(1, "A1", Side::Sell, "AAPL", make_price(100, 0), 100));
  engine.process(enter(1, "M1", Side::Buy, "MSFT", make_price(100, 0), 100));
  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);  // two Accepted, no executions across symbols
  EXPECT_NE(std::get_if<ouch::Accepted>(&rs[0].msg), nullptr);
  EXPECT_NE(std::get_if<ouch::Accepted>(&rs[1].msg), nullptr);
}

TEST_F(EngineTest, DuplicateTokenRejected) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(100, 0), 100));
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(99, 0), 100));

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);
  const auto* rej = std::get_if<ouch::Rejected>(&rs[1].msg);
  ASSERT_NE(rej, nullptr);
  EXPECT_EQ(ouch::token_view(rej->token), "T1");
}

TEST_F(EngineTest, SameTokenDifferentClientsIsFine) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(99, 0), 100));
  engine.process(enter(2, "T1", Side::Buy, "AAPL", make_price(98, 0), 100));
  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);
  EXPECT_NE(std::get_if<ouch::Accepted>(&rs[1].msg), nullptr);
}

TEST_F(EngineTest, BadQtyAndPriceRejected) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(100, 0), 0));
  engine.process(enter(1, "T2", Side::Buy, "AAPL", 0, 100));
  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);
  EXPECT_NE(std::get_if<ouch::Rejected>(&rs[0].msg), nullptr);
  EXPECT_NE(std::get_if<ouch::Rejected>(&rs[1].msg), nullptr);
}

TEST_F(EngineTest, CancelByToken) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(100, 0), 500));
  ouch::CancelOrder c{};
  c.token = ouch::make_token("T1");
  c.shares = 0;
  engine.process(Command{1, c});

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);
  const auto* can = std::get_if<ouch::Canceled>(&rs[1].msg);
  ASSERT_NE(can, nullptr);
  EXPECT_EQ(can->decrement_shares, 500u);
  EXPECT_EQ(can->reason, 'U');

  const auto evs = events();
  ASSERT_EQ(evs.size(), 2u);
  const auto* ce = std::get_if<CanceledEvent>(&evs[1].ev);
  ASSERT_NE(ce, nullptr);
  EXPECT_TRUE(ce->removed);
}

TEST_F(EngineTest, CancelUnknownTokenIgnored) {
  ouch::CancelOrder c{};
  c.token = ouch::make_token("NOPE");
  engine.process(Command{1, c});
  EXPECT_TRUE(responses().empty());
}

TEST_F(EngineTest, CancelSomeoneElsesTokenIgnored) {
  engine.process(enter(1, "T1", Side::Buy, "AAPL", make_price(100, 0), 500));
  ouch::CancelOrder c{};
  c.token = ouch::make_token("T1");
  engine.process(Command{2, c});
  EXPECT_EQ(responses().size(), 1u);  // only the Accepted
}

TEST_F(EngineTest, ReplaceOrder) {
  engine.process(enter(1, "OLD", Side::Buy, "AAPL", make_price(100, 0), 500));
  ouch::ReplaceOrder r{};
  r.existing_token = ouch::make_token("OLD");
  r.replacement_token = ouch::make_token("NEW");
  r.shares = 300;
  r.price = make_price(99, 0);
  r.display = 'Y';
  r.intermarket_sweep = 'N';
  engine.process(Command{1, r});

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 2u);
  const auto* rep = std::get_if<ouch::Replaced>(&rs[1].msg);
  ASSERT_NE(rep, nullptr);
  EXPECT_EQ(ouch::token_view(rep->replacement_token), "NEW");
  EXPECT_EQ(ouch::token_view(rep->previous_token), "OLD");
  EXPECT_EQ(rep->shares, 300u);

  // NEW is now live and cancelable.
  ouch::CancelOrder c{};
  c.token = ouch::make_token("NEW");
  engine.process(Command{1, c});
  const auto rs2 = responses();
  ASSERT_EQ(rs2.size(), 1u);
  EXPECT_NE(std::get_if<ouch::Canceled>(&rs2[0].msg), nullptr);
}

TEST_F(EngineTest, ReplaceIntoCrossExecutesWithoutOuchCancel) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(101, 0), 100));
  engine.process(enter(2, "B1", Side::Buy, "AAPL", make_price(99, 0), 100));
  ouch::ReplaceOrder r{};
  r.existing_token = ouch::make_token("B1");
  r.replacement_token = ouch::make_token("B2");
  r.shares = 100;
  r.price = make_price(101, 0);
  engine.process(Command{2, r});

  const auto rs = responses();
  // Accepted, Accepted, Replaced(B2), Executed(S1), Executed(B2) — and no
  // OUCH Canceled for the replaced-away B1.
  ASSERT_EQ(rs.size(), 5u);
  EXPECT_NE(std::get_if<ouch::Replaced>(&rs[2].msg), nullptr);
  const auto* ex1 = std::get_if<ouch::Executed>(&rs[3].msg);
  const auto* ex2 = std::get_if<ouch::Executed>(&rs[4].msg);
  ASSERT_NE(ex1, nullptr);
  ASSERT_NE(ex2, nullptr);
  EXPECT_EQ(ouch::token_view(ex2->token), "B2");
}

TEST_F(EngineTest, ReplaceUnknownOrExecutedTokenIgnored) {
  ouch::ReplaceOrder r{};
  r.existing_token = ouch::make_token("NOPE");
  r.replacement_token = ouch::make_token("NEW");
  r.shares = 100;
  r.price = make_price(100, 0);
  engine.process(Command{1, r});
  EXPECT_TRUE(responses().empty());
}

// ---------------------------------------------------------------- time in force

TEST_F(EngineTest, IocPartialFillsThenCancelsRemainderWithoutResting) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 100));
  engine.process(
      enter_tif(2, "B1", Side::Buy, "AAPL", make_price(100, 0), 300,
                ouch::kTifIOC));

  const auto rs = responses();
  // Accepted(S1), Accepted(B1), Executed(S1), Executed(B1), Canceled(B1 rem).
  ASSERT_EQ(rs.size(), 5u);
  EXPECT_NE(std::get_if<ouch::Executed>(&rs[2].msg), nullptr);
  EXPECT_NE(std::get_if<ouch::Executed>(&rs[3].msg), nullptr);
  const auto* can = std::get_if<ouch::Canceled>(&rs[4].msg);
  ASSERT_NE(can, nullptr);
  EXPECT_EQ(rs[4].client_id, 2u);
  EXPECT_EQ(can->decrement_shares, 200u);  // 300 - 100 filled
  EXPECT_EQ(can->reason, 'I');

  // Nothing from the IOC order rests: both sides of the book are empty.
  const auto snap = engine.book_snapshot(make_symbol("AAPL"));
  EXPECT_TRUE(snap.bids.empty());
  EXPECT_TRUE(snap.asks.empty());
}

TEST_F(EngineTest, IocFullyFilledEmitsNoCancel) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 300));
  engine.process(
      enter_tif(2, "B1", Side::Buy, "AAPL", make_price(100, 0), 300,
                ouch::kTifIOC));

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 4u);  // Accepted x2, Executed x2, no Canceled
  EXPECT_EQ(std::get_if<ouch::Canceled>(&rs[3].msg), nullptr);
  EXPECT_NE(std::get_if<ouch::Executed>(&rs[3].msg), nullptr);
}

TEST_F(EngineTest, FillOrKillKilledWhenLiquidityInsufficient) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 100));
  // FOK == IOC with min_qty == shares. Only 100 available, want 300.
  engine.process(
      enter_tif(2, "B1", Side::Buy, "AAPL", make_price(100, 0), 300,
                ouch::kTifIOC, /*min_qty=*/300));

  const auto rs = responses();
  // Accepted(S1), Accepted(B1), Canceled(B1) — no executions at all.
  ASSERT_EQ(rs.size(), 3u);
  const auto* can = std::get_if<ouch::Canceled>(&rs[2].msg);
  ASSERT_NE(can, nullptr);
  EXPECT_EQ(can->decrement_shares, 300u);
  EXPECT_EQ(can->reason, 'D');

  // The resting sell is untouched (the kill never took any liquidity).
  const auto snap = engine.book_snapshot(make_symbol("AAPL"));
  ASSERT_EQ(snap.asks.size(), 1u);
  EXPECT_EQ(snap.asks[0].price, make_price(100, 0));
  EXPECT_EQ(snap.asks[0].orders[0].qty, 100u);
}

TEST_F(EngineTest, FillOrKillFillsWhenLiquiditySuffices) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 300));
  engine.process(
      enter_tif(2, "B1", Side::Buy, "AAPL", make_price(100, 0), 300,
                ouch::kTifIOC, /*min_qty=*/300));

  const auto rs = responses();
  ASSERT_EQ(rs.size(), 4u);  // Accepted x2, Executed x2
  EXPECT_NE(std::get_if<ouch::Executed>(&rs[3].msg), nullptr);
  const auto snap = engine.book_snapshot(make_symbol("AAPL"));
  EXPECT_TRUE(snap.asks.empty());
}

TEST_F(EngineTest, MarketOrderWalksLevelsAndCancelsRemainder) {
  engine.process(enter(1, "S1", Side::Sell, "AAPL", make_price(100, 0), 100));
  engine.process(enter(1, "S2", Side::Sell, "AAPL", make_price(101, 0), 100));
  engine.process(enter_tif(2, "B1", Side::Buy, "AAPL", ouch::kMarketPrice, 250,
                           ouch::kTifDay));

  const auto rs = responses();
  // Accepted x3, then four Executed (two levels x two owners), then Canceled.
  ASSERT_EQ(rs.size(), 8u);
  const auto* ex_a = std::get_if<ouch::Executed>(&rs[3].msg);
  const auto* ex_b = std::get_if<ouch::Executed>(&rs[5].msg);
  ASSERT_NE(ex_a, nullptr);
  ASSERT_NE(ex_b, nullptr);
  EXPECT_EQ(ex_a->execution_price, make_price(100, 0));
  EXPECT_EQ(ex_b->execution_price, make_price(101, 0));  // walked up a level
  const auto* can = std::get_if<ouch::Canceled>(&rs[7].msg);
  ASSERT_NE(can, nullptr);
  EXPECT_EQ(can->decrement_shares, 50u);  // 250 - 200 available
  EXPECT_EQ(can->reason, 'I');

  const auto snap = engine.book_snapshot(make_symbol("AAPL"));
  EXPECT_TRUE(snap.asks.empty());
  EXPECT_TRUE(snap.bids.empty());
}

// Exercises the low-latency path: a lock-free CommandChannel consumed by the
// engine's busy-poll loop on its own thread, with commands pushed from this
// (producer) thread. Verifies correct processing and clean shutdown — and
// gives ThreadSanitizer the ring + busy-poll + stop path to check.
TEST(EngineLowLatency, BusyPollRingProcessesAllOrdersAndStops) {
  CommandChannel in{/*lock_free=*/true};
  MpscQueue<ClientResponse> to_gateway;
  MpscQueue<MarketEvent> to_feed;
  Engine engine{in, to_gateway, to_feed};
  engine.start();  // busy-polls because the channel is lock-free

  constexpr int kN = 500;
  for (int i = 0; i < kN; ++i)
    in.push(enter(1, "T" + std::to_string(i), (i & 1) ? Side::Buy : Side::Sell,
                  "AAPL", make_price(100, 0), 100));

  // Every order yields at least an Accepted; collect until we've seen kN.
  // Yield when idle so hammering the response queue's lock doesn't starve the
  // engine's pushes; a generous deadline guards against a genuine stall.
  int accepted = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (accepted < kN && std::chrono::steady_clock::now() < deadline) {
    const auto batch = to_gateway.drain();
    if (batch.empty()) {
      std::this_thread::yield();
      continue;
    }
    for (const auto& r : batch)
      if (std::holds_alternative<ouch::Accepted>(r.msg)) ++accepted;
  }

  engine.stop();
  EXPECT_EQ(accepted, kN);
}

TEST(MpscQueueTest, PushDrainAcrossThreads) {
  MpscQueue<int> q;
  std::thread producer([&] {
    for (int i = 0; i < 1000; ++i) q.push(int{i});
  });
  std::vector<int> got;
  while (got.size() < 1000) {
    for (int v : q.drain()) got.push_back(v);
  }
  producer.join();
  ASSERT_EQ(got.size(), 1000u);
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(got[static_cast<std::size_t>(i)], i);
}

TEST(MpscQueueTest, NotifyFiresOnPush) {
  MpscQueue<int> q;
  int notified = 0;
  q.set_notify([&] { ++notified; });
  q.push(1);
  q.push(2);
  EXPECT_EQ(notified, 2);
  EXPECT_EQ(q.drain().size(), 2u);
}

}  // namespace
}  // namespace nsq::engine

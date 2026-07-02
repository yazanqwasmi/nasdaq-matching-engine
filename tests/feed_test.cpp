// Feed tests: MarketEvent -> ITCH translation, and the full publisher
// round-trip — engine events through MoldUDP64 over real UDP into the
// client-side ITCH book builder, whose reconstruction must equal the
// engine's own book. (The test uses unicast-to-localhost for determinism;
// the apps use multicast — same sendto path.)
#include "client/itch_book.hpp"
#include "engine/engine.hpp"
#include "feed/feed.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <random>

namespace nsq {
namespace {

engine::Command enter(std::uint64_t client, std::string_view token, Side side,
                      Price price, Qty shares) {
  ouch::EnterOrder m{};
  m.token = ouch::make_token(token);
  m.side = side;
  m.shares = shares;
  m.stock = make_symbol("AAPL");
  m.price = price;
  m.tif = 99999;
  m.display = 'Y';
  return {client, m};
}

TEST(FeedTranslate, AddedEventBecomesItchAddOrder) {
  engine::MarketEvent ev{123, make_symbol("AAPL"),
                         AddedEvent{7, Side::Buy, make_price(100, 0), 500}};
  const auto bytes = feed::to_itch(ev, /*locate=*/3);
  const auto msg = itch::decode(bytes.data(), bytes.size());
  ASSERT_TRUE(msg.has_value());
  const auto* add = std::get_if<itch::AddOrder>(&*msg);
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->stock_locate, 3u);
  EXPECT_EQ(add->timestamp, 123u);
  EXPECT_EQ(add->order_ref, 7u);
  EXPECT_EQ(add->side, Side::Buy);
  EXPECT_EQ(add->shares, 500u);
  EXPECT_EQ(symbol_view(add->stock), "AAPL");
  EXPECT_EQ(add->price, make_price(100, 0));
}

TEST(FeedTranslate, ExecCancelDeleteReplace) {
  const Symbol sym = make_symbol("AAPL");

  const auto exec_bytes =
      feed::to_itch({1, sym, ExecutedEvent{7, 8, Side::Buy, make_price(100, 0), 50, 99}}, 1);
  const auto exec = itch::decode(exec_bytes.data(), exec_bytes.size());
  const auto* e = std::get_if<itch::OrderExecuted>(&*exec);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->order_ref, 7u);
  EXPECT_EQ(e->executed_shares, 50u);
  EXPECT_EQ(e->match_number, 99u);

  const auto partial =
      feed::to_itch({2, sym, CanceledEvent{7, 30, false}}, 1);
  const auto partial_msg = itch::decode(partial.data(), partial.size());
  const auto* x = std::get_if<itch::OrderCancel>(&*partial_msg);
  ASSERT_NE(x, nullptr);
  EXPECT_EQ(x->canceled_shares, 30u);

  const auto full = feed::to_itch({3, sym, CanceledEvent{7, 100, true}}, 1);
  const auto full_msg = itch::decode(full.data(), full.size());
  const auto* d = std::get_if<itch::OrderDelete>(&*full_msg);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->order_ref, 7u);

  const auto rep = feed::to_itch(
      {4, sym, ReplacedEvent{7, 9, Side::Buy, make_price(99, 0), 200}}, 1);
  const auto rep_msg = itch::decode(rep.data(), rep.size());
  const auto* u = std::get_if<itch::OrderReplace>(&*rep_msg);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->original_ref, 7u);
  EXPECT_EQ(u->new_ref, 9u);
  EXPECT_EQ(u->shares, 200u);
}

TEST(ItchBookBuilder, ReconstructsFromMessages) {
  client::ItchBookBuilder builder;
  const Symbol sym = make_symbol("AAPL");

  itch::AddOrder a{};
  a.order_ref = 1;
  a.side = Side::Buy;
  a.shares = 100;
  a.stock = sym;
  a.price = make_price(100, 0);
  builder.on_message(a);

  a.order_ref = 2;
  a.side = Side::Sell;
  a.price = make_price(101, 0);
  builder.on_message(a);

  itch::OrderExecuted e{};
  e.order_ref = 1;
  e.executed_shares = 40;
  e.match_number = 1;
  builder.on_message(e);

  const auto snap = builder.snapshot(sym);
  ASSERT_EQ(snap.bids.size(), 1u);
  EXPECT_EQ(snap.bids[0].orders[0].qty, 60u);
  ASSERT_EQ(snap.asks.size(), 1u);

  e.executed_shares = 60;  // exhausts order 1 -> level disappears
  builder.on_message(e);
  EXPECT_TRUE(builder.snapshot(sym).bids.empty());
}

TEST(FeedEndToEnd, UdpRoundTripReconstructionEqualsEngineBook) {
  // Receiving socket on an ephemeral localhost port.
  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(rx, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(::bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
  socklen_t alen = sizeof addr;
  ::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &alen);
  timeval tv{5, 0};
  ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  engine::MpscQueue<engine::Command> in;
  engine::MpscQueue<engine::ClientResponse> to_gateway;
  engine::MpscQueue<engine::MarketEvent> to_feed;
  engine::Engine eng{in, to_gateway, to_feed};

  feed::FeedConfig cfg;
  cfg.dest_ip = "127.0.0.1";
  cfg.dest_port = ntohs(addr.sin_port);
  cfg.session = "TESTSESS";
  feed::FeedPublisher pub{to_feed, cfg};
  pub.start();

  // Drive a random session through the engine synchronously.
  std::mt19937_64 rng(2024);
  std::uniform_int_distribution<Price> tick(-20, 20);
  std::uniform_int_distribution<Qty> qty(1, 500);
  for (int i = 0; i < 500; ++i) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    eng.process(enter(1, "T" + std::to_string(i), side,
                      make_price(100, 0) + tick(rng) * make_price(0, 100),
                      qty(rng)));
  }
  to_gateway.drain();
  pub.stop();  // drains remaining events, flushes, sends end-of-session

  // Receive datagrams until end-of-session; feed them to the builder.
  client::ItchBookBuilder builder;
  bool eos = false;
  while (!eos) {
    std::uint8_t buf[2048];
    const ssize_t n = ::recvfrom(rx, buf, sizeof buf, 0, nullptr, nullptr);
    ASSERT_GT(n, 0) << "timed out waiting for end-of-session";
    eos = builder.on_mold_packet(buf, static_cast<std::size_t>(n));
  }
  ::close(rx);

  EXPECT_FALSE(builder.had_gap());
  EXPECT_GT(builder.message_count(), 0u);

  const auto engine_snap = eng.book_snapshot(make_symbol("AAPL"));
  const auto feed_snap = builder.snapshot(make_symbol("AAPL"));
  ASSERT_FALSE(engine_snap.bids.empty());
  EXPECT_EQ(engine_snap, feed_snap);
}

}  // namespace
}  // namespace nsq

// OUCH 4.2 codec tests: golden byte layouts (field offsets from the spec)
// and encode/decode round-trips for the documented subset.
#include "ouch/ouch.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace nsq::ouch {
namespace {

TEST(OuchToken, PadAndTrim) {
  const Token t = make_token("ABC123");
  EXPECT_EQ(t.size(), 14u);
  EXPECT_EQ(t[5], '3');
  EXPECT_EQ(t[6], ' ');
  EXPECT_EQ(token_view(t), "ABC123");
}

TEST(OuchEnterOrder, GoldenLayout) {
  EnterOrder m{};
  m.token = make_token("TKN1");
  m.side = Side::Buy;
  m.shares = 100;
  m.stock = make_symbol("AAPL");
  m.price = make_price(185, 1500);  // 1851500 = 0x001C406C
  m.tif = 99999;                    // market hours
  m.firm = {'F', 'R', 'M', 'A'};
  m.display = 'Y';
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.min_qty = 0;
  m.cross_type = 'N';
  m.customer_type = 'R';

  std::array<std::uint8_t, kEnterOrderSize> buf{};
  encode(m, buf.data());

  EXPECT_EQ(buf[0], 'O');
  EXPECT_EQ(buf[1], 'T');           // token starts at offset 1
  EXPECT_EQ(buf[5], ' ');           // token space padding
  EXPECT_EQ(buf[15], 'B');          // side at 15
  EXPECT_EQ(buf[16], 0x00);         // shares (BE32) at 16..19
  EXPECT_EQ(buf[19], 100);
  EXPECT_EQ(buf[20], 'A');          // stock at 20..27
  EXPECT_EQ(buf[27], ' ');
  EXPECT_EQ(buf[28], 0x00);         // price (BE32) at 28..31
  EXPECT_EQ(buf[29], 0x1C);
  EXPECT_EQ(buf[30], 0x40);
  EXPECT_EQ(buf[31], 0x6C);
  EXPECT_EQ(buf[40], 'Y');          // display at 40
  EXPECT_EQ(buf[48], 'R');          // customer type at 48

  const auto d = decode_enter_order(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(token_view(d->token), "TKN1");
  EXPECT_EQ(d->side, Side::Buy);
  EXPECT_EQ(d->shares, 100u);
  EXPECT_EQ(symbol_view(d->stock), "AAPL");
  EXPECT_EQ(d->price, make_price(185, 1500));
  EXPECT_EQ(d->tif, 99999u);
  EXPECT_EQ(d->display, 'Y');
}

TEST(OuchEnterOrder, DecodeWrongSizeFails) {
  std::array<std::uint8_t, kEnterOrderSize> buf{};
  buf[0] = 'O';
  EXPECT_FALSE(decode_enter_order(buf.data(), buf.size() - 1).has_value());
}

TEST(OuchEnterOrder, DecodeWrongTypeFails) {
  std::array<std::uint8_t, kEnterOrderSize> buf{};
  buf[0] = 'X';
  EXPECT_FALSE(decode_enter_order(buf.data(), buf.size()).has_value());
}

TEST(OuchCancel, RoundTrip) {
  CancelOrder m{};
  m.token = make_token("TKN1");
  m.shares = 0;  // full cancel

  std::array<std::uint8_t, kCancelOrderSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'X');
  static_assert(kCancelOrderSize == 19);

  const auto d = decode_cancel_order(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(token_view(d->token), "TKN1");
  EXPECT_EQ(d->shares, 0u);
}

TEST(OuchReplace, RoundTrip) {
  ReplaceOrder m{};
  m.existing_token = make_token("OLD");
  m.replacement_token = make_token("NEW");
  m.shares = 250;
  m.price = make_price(50, 0);
  m.tif = 0;
  m.display = 'Y';
  m.intermarket_sweep = 'N';
  m.min_qty = 0;

  std::array<std::uint8_t, kReplaceOrderSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'U');
  static_assert(kReplaceOrderSize == 47);

  const auto d = decode_replace_order(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(token_view(d->existing_token), "OLD");
  EXPECT_EQ(token_view(d->replacement_token), "NEW");
  EXPECT_EQ(d->shares, 250u);
  EXPECT_EQ(d->price, make_price(50, 0));
}

TEST(OuchAccepted, RoundTrip) {
  Accepted m{};
  m.timestamp = 34200'000'000'000ULL;  // 09:30 in ns
  m.token = make_token("TKN1");
  m.side = Side::Sell;
  m.shares = 500;
  m.stock = make_symbol("MSFT");
  m.price = make_price(430, 100);
  m.tif = 99999;
  m.firm = {'F', 'R', 'M', 'A'};
  m.display = 'Y';
  m.order_ref = 777;
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.min_qty = 0;
  m.cross_type = 'N';
  m.order_state = 'L';  // live

  std::array<std::uint8_t, kAcceptedSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'A');
  static_assert(kAcceptedSize == 66);

  const auto d = decode_accepted(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->timestamp, m.timestamp);
  EXPECT_EQ(d->side, Side::Sell);
  EXPECT_EQ(d->order_ref, 777u);
  EXPECT_EQ(d->order_state, 'L');
  EXPECT_EQ(d->price, make_price(430, 100));
}

TEST(OuchExecuted, RoundTrip) {
  Executed m{};
  m.timestamp = 1;
  m.token = make_token("TKN1");
  m.executed_shares = 100;
  m.execution_price = make_price(10, 0);
  m.liquidity_flag = 'A';
  m.match_number = 42;

  std::array<std::uint8_t, kExecutedSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'E');
  static_assert(kExecutedSize == 40);

  const auto d = decode_executed(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->executed_shares, 100u);
  EXPECT_EQ(d->match_number, 42u);
}

TEST(OuchCanceled, RoundTrip) {
  Canceled m{};
  m.timestamp = 2;
  m.token = make_token("TKN9");
  m.decrement_shares = 300;
  m.reason = 'U';  // user requested

  std::array<std::uint8_t, kCanceledSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'C');
  static_assert(kCanceledSize == 28);

  const auto d = decode_canceled(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->decrement_shares, 300u);
  EXPECT_EQ(d->reason, 'U');
}

TEST(OuchReplaced, RoundTrip) {
  Replaced m{};
  m.timestamp = 3;
  m.replacement_token = make_token("NEW");
  m.side = Side::Buy;
  m.shares = 100;
  m.stock = make_symbol("AAPL");
  m.price = make_price(100, 0);
  m.tif = 99999;
  m.firm = {'F', 'R', 'M', 'A'};
  m.display = 'Y';
  m.order_ref = 12;
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.min_qty = 0;
  m.cross_type = 'N';
  m.order_state = 'L';
  m.previous_token = make_token("OLD");

  std::array<std::uint8_t, kReplacedSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'U');
  static_assert(kReplacedSize == 80);

  const auto d = decode_replaced(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(token_view(d->replacement_token), "NEW");
  EXPECT_EQ(token_view(d->previous_token), "OLD");
}

TEST(OuchRejected, RoundTrip) {
  Rejected m{};
  m.timestamp = 4;
  m.token = make_token("BAD");
  m.reason = 'X';

  std::array<std::uint8_t, kRejectedSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'J');
  static_assert(kRejectedSize == 24);

  const auto d = decode_rejected(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->reason, 'X');
}

TEST(OuchSystemEvent, RoundTrip) {
  SystemEvent m{};
  m.timestamp = 5;
  m.event_code = 'S';  // start of day

  std::array<std::uint8_t, kSystemEventSize> buf{};
  encode(m, buf.data());
  EXPECT_EQ(buf[0], 'S');
  static_assert(kSystemEventSize == 10);

  const auto d = decode_system_event(buf.data(), buf.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->event_code, 'S');
}

}  // namespace
}  // namespace nsq::ouch

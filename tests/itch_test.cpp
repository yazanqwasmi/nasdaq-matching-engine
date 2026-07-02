// ITCH 5.0 codec tests: golden byte layout for Add Order (spec offsets) and
// encode/decode round-trips for the documented subset.
#include "itch/itch.hpp"

#include <gtest/gtest.h>

#include <array>

namespace nsq::itch {
namespace {

template <typename T>
T roundtrip(const T& m) {
  std::array<std::uint8_t, 64> buf{};
  encode(m, buf.data());
  const auto decoded = decode(buf.data(), T::kSize);
  EXPECT_TRUE(decoded.has_value());
  const T* out = std::get_if<T>(&*decoded);
  EXPECT_NE(out, nullptr);
  return *out;
}

TEST(ItchAddOrder, GoldenLayout) {
  AddOrder m{};
  m.stock_locate = 0x0102;
  m.tracking = 0x0304;
  m.timestamp = 0x0000'AABB'CCDD'EEFFULL;
  m.order_ref = 777;
  m.side = Side::Buy;
  m.shares = 100;
  m.stock = make_symbol("AAPL");
  m.price = make_price(185, 1500);

  std::array<std::uint8_t, AddOrder::kSize> buf{};
  static_assert(AddOrder::kSize == 36);
  encode(m, buf.data());

  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[1], 0x01);  // stock locate BE16 at 1
  EXPECT_EQ(buf[2], 0x02);
  EXPECT_EQ(buf[3], 0x03);  // tracking at 3
  EXPECT_EQ(buf[5], 0xAA);  // 48-bit timestamp at 5..10
  EXPECT_EQ(buf[10], 0xFF);
  EXPECT_EQ(buf[18], 777 & 0xFF);  // order ref BE64 at 11..18
  EXPECT_EQ(buf[19], 'B');         // side at 19
  EXPECT_EQ(buf[23], 100);         // shares BE32 at 20..23
  EXPECT_EQ(buf[24], 'A');         // stock at 24..31
  EXPECT_EQ(buf[31], ' ');
  EXPECT_EQ(buf[32], 0x00);        // price BE32 at 32..35
  EXPECT_EQ(buf[33], 0x1C);
  EXPECT_EQ(buf[34], 0x40);
  EXPECT_EQ(buf[35], 0x6C);

  const AddOrder out = roundtrip(m);
  EXPECT_EQ(out.order_ref, 777u);
  EXPECT_EQ(out.timestamp, m.timestamp);
  EXPECT_EQ(symbol_view(out.stock), "AAPL");
  EXPECT_EQ(out.price, make_price(185, 1500));
}

TEST(ItchMessages, RoundTrips) {
  SystemEvent se{};
  se.timestamp = 123;
  se.event_code = 'Q';  // market open
  static_assert(SystemEvent::kSize == 12);
  EXPECT_EQ(roundtrip(se).event_code, 'Q');

  StockDirectory sd{};
  sd.stock = make_symbol("MSFT");
  sd.round_lot_size = 100;
  sd.market_category = 'Q';
  static_assert(StockDirectory::kSize == 39);
  const auto sd2 = roundtrip(sd);
  EXPECT_EQ(symbol_view(sd2.stock), "MSFT");
  EXPECT_EQ(sd2.round_lot_size, 100u);

  TradingAction ta{};
  ta.stock = make_symbol("MSFT");
  ta.trading_state = 'T';  // trading
  static_assert(TradingAction::kSize == 25);
  EXPECT_EQ(roundtrip(ta).trading_state, 'T');

  AddOrderMpid f{};
  f.add.order_ref = 5;
  f.add.side = Side::Sell;
  f.add.shares = 10;
  f.add.stock = make_symbol("X");
  f.add.price = make_price(1, 0);
  f.attribution = {'V', 'I', 'R', 'T'};
  static_assert(AddOrderMpid::kSize == 40);
  const auto f2 = roundtrip(f);
  EXPECT_EQ(f2.add.order_ref, 5u);
  EXPECT_EQ(f2.attribution[0], 'V');

  OrderExecuted ex{};
  ex.order_ref = 9;
  ex.executed_shares = 50;
  ex.match_number = 1000;
  static_assert(OrderExecuted::kSize == 31);
  EXPECT_EQ(roundtrip(ex).match_number, 1000u);

  OrderExecutedWithPrice exp{};
  exp.exec.order_ref = 9;
  exp.exec.executed_shares = 25;
  exp.exec.match_number = 1001;
  exp.printable = 'Y';
  exp.execution_price = make_price(10, 5000);
  static_assert(OrderExecutedWithPrice::kSize == 36);
  EXPECT_EQ(roundtrip(exp).execution_price, make_price(10, 5000));

  OrderCancel oc{};
  oc.order_ref = 4;
  oc.canceled_shares = 75;
  static_assert(OrderCancel::kSize == 23);
  EXPECT_EQ(roundtrip(oc).canceled_shares, 75u);

  OrderDelete od{};
  od.order_ref = 4;
  static_assert(OrderDelete::kSize == 19);
  EXPECT_EQ(roundtrip(od).order_ref, 4u);

  OrderReplace orp{};
  orp.original_ref = 4;
  orp.new_ref = 6;
  orp.shares = 200;
  orp.price = make_price(99, 0);
  static_assert(OrderReplace::kSize == 35);
  const auto orp2 = roundtrip(orp);
  EXPECT_EQ(orp2.new_ref, 6u);
  EXPECT_EQ(orp2.price, make_price(99, 0));

  Trade tr{};
  tr.order_ref = 0;
  tr.side = Side::Buy;
  tr.shares = 100;
  tr.stock = make_symbol("AAPL");
  tr.price = make_price(185, 0);
  tr.match_number = 77;
  static_assert(Trade::kSize == 44);
  EXPECT_EQ(roundtrip(tr).match_number, 77u);
}

TEST(ItchDecode, RejectsWrongSizeAndUnknownType) {
  std::array<std::uint8_t, 64> buf{};
  buf[0] = 'A';
  EXPECT_FALSE(decode(buf.data(), AddOrder::kSize - 1).has_value());
  buf[0] = '?';
  EXPECT_FALSE(decode(buf.data(), 12).has_value());
}

}  // namespace
}  // namespace nsq::itch

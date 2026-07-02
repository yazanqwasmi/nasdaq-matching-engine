#include "common/clock.hpp"
#include "common/endian.hpp"
#include "common/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace nsq {
namespace {

TEST(Endian, Be16KnownBytes) {
  std::array<std::uint8_t, 2> buf{};
  put_be16(buf.data(), 0x1234);
  EXPECT_EQ(buf[0], 0x12);
  EXPECT_EQ(buf[1], 0x34);
  EXPECT_EQ(get_be16(buf.data()), 0x1234);
}

TEST(Endian, Be32KnownBytes) {
  std::array<std::uint8_t, 4> buf{};
  put_be32(buf.data(), 0xDEADBEEF);
  EXPECT_EQ(buf[0], 0xDE);
  EXPECT_EQ(buf[1], 0xAD);
  EXPECT_EQ(buf[2], 0xBE);
  EXPECT_EQ(buf[3], 0xEF);
  EXPECT_EQ(get_be32(buf.data()), 0xDEADBEEF);
}

TEST(Endian, Be48KnownBytes) {
  // ITCH 5.0 timestamps are 6-byte big-endian nanoseconds since midnight.
  std::array<std::uint8_t, 6> buf{};
  put_be48(buf.data(), 0x0000'AABB'CCDD'EEFFULL & 0x0000'FFFF'FFFF'FFFFULL);
  EXPECT_EQ(buf[0], 0xAA);
  EXPECT_EQ(buf[5], 0xFF);
  EXPECT_EQ(get_be48(buf.data()), 0x0000'AABB'CCDD'EEFFULL);
}

TEST(Endian, Be64RoundTrip) {
  std::array<std::uint8_t, 8> buf{};
  const std::uint64_t v = 0x0102'0304'0506'0708ULL;
  put_be64(buf.data(), v);
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[7], 0x08);
  EXPECT_EQ(get_be64(buf.data()), v);
}

TEST(Price, MakeAndFormat) {
  // Prices are int64 in units of 1/10000 dollar.
  const Price p = make_price(185, 1500);  // $185.15
  EXPECT_EQ(p, 1851500);
  EXPECT_EQ(price_to_string(p), "185.1500");
  EXPECT_EQ(price_to_string(make_price(0, 5)), "0.0005");
}

TEST(Symbol, PadAndTrim) {
  const Symbol s = make_symbol("AAPL");
  EXPECT_EQ(s.size(), 8u);
  EXPECT_EQ(s[3], 'L');
  EXPECT_EQ(s[4], ' ');  // right-padded with spaces per NASDAQ convention
  EXPECT_EQ(s[7], ' ');
  EXPECT_EQ(symbol_view(s), "AAPL");
}

TEST(Symbol, EightCharSymbolUsesAllBytes) {
  const Symbol s = make_symbol("ABCDEFGH");
  EXPECT_EQ(symbol_view(s), "ABCDEFGH");
}

TEST(Clock, NsSinceMidnightWithinOneDay) {
  const std::uint64_t ns = ns_since_midnight();
  EXPECT_LT(ns, 24ULL * 60 * 60 * 1'000'000'000);
  const std::uint64_t ns2 = ns_since_midnight();
  EXPECT_GE(ns2, ns);
}

}  // namespace
}  // namespace nsq

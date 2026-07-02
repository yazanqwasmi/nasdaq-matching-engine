// End-to-end raw-ITCH processing: symbol filtering, skip counting, and
// book reconstruction from a synthetic file.
#include "client/itch_file_processor.hpp"
#include "itch/raw_itch.hpp"

#include <gtest/gtest.h>

#include <cstdio>

namespace nsq::client {
namespace {

template <typename T>
void put(std::vector<std::uint8_t>& stream, const T& m) {
  std::vector<std::uint8_t> buf(T::kSize);
  itch::encode(m, buf.data());
  itch::append_raw(stream, buf.data(), buf.size());
}

std::FILE* tmpfile_with(const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::tmpfile();
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::rewind(f);
  return f;
}

std::vector<std::uint8_t> synthetic_session() {
  std::vector<std::uint8_t> s;
  itch::AddOrder a{};
  a.order_ref = 1;
  a.side = Side::Buy;
  a.shares = 100;
  a.stock = make_symbol("AAPL");
  a.price = make_price(100, 0);
  put(s, a);
  a.order_ref = 2;
  a.stock = make_symbol("MSFT");
  put(s, a);
  itch::OrderExecuted e{};
  e.order_ref = 1;
  e.executed_shares = 40;
  e.match_number = 7;
  put(s, e);
  std::vector<std::uint8_t> noii(50, 0);  // NOII: framed, not decoded
  noii[0] = 'I';
  itch::append_raw(s, noii.data(), noii.size());
  return s;
}

TEST(ProcessRawItch, UnfilteredBuildsBothBooksAndCountsSkips) {
  std::FILE* f = tmpfile_with(synthetic_session());
  ItchBookBuilder builder;
  ItchFileStats stats;
  ASSERT_TRUE(process_raw_itch(f, {}, 0, builder, stats));
  std::fclose(f);

  EXPECT_EQ(stats.total, 4u);
  EXPECT_EQ(stats.decoded[static_cast<unsigned char>('A')], 2u);
  EXPECT_EQ(stats.decoded[static_cast<unsigned char>('E')], 1u);
  EXPECT_EQ(stats.skipped[static_cast<unsigned char>('I')], 1u);
  EXPECT_EQ(stats.filtered, 0u);
  EXPECT_EQ(builder.symbols().size(), 2u);
  const auto snap = builder.snapshot(make_symbol("AAPL"));
  ASSERT_EQ(snap.bids.size(), 1u);
  EXPECT_EQ(snap.bids[0].orders[0].qty, 60u);
}

TEST(ProcessRawItch, SymbolFilterDropsOtherAdds) {
  std::FILE* f = tmpfile_with(synthetic_session());
  ItchBookBuilder builder;
  ItchFileStats stats;
  ASSERT_TRUE(process_raw_itch(f, {make_symbol("AAPL")}, 0, builder, stats));
  std::fclose(f);

  EXPECT_EQ(stats.filtered, 1u);  // the MSFT add
  EXPECT_EQ(builder.symbols().size(), 1u);
}

TEST(ProcessRawItch, MaxMessagesCapStopsEarly) {
  std::FILE* f = tmpfile_with(synthetic_session());
  ItchBookBuilder builder;
  ItchFileStats stats;
  ASSERT_TRUE(process_raw_itch(f, {}, 2, builder, stats));
  std::fclose(f);
  EXPECT_EQ(stats.total, 2u);
}

TEST(ProcessRawItch, FramingErrorReported) {
  std::vector<std::uint8_t> s;
  std::vector<std::uint8_t> bad(35, 0);
  bad[0] = 'A';  // AddOrder must be 36
  itch::append_raw(s, bad.data(), bad.size());
  std::FILE* f = tmpfile_with(s);
  ItchBookBuilder builder;
  ItchFileStats stats;
  EXPECT_FALSE(process_raw_itch(f, {}, 0, builder, stats));
  EXPECT_FALSE(stats.error.empty());
  std::fclose(f);
}

}  // namespace
}  // namespace nsq::client

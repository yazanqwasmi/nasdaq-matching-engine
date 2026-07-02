// Raw-ITCH framing: full message-size table and length-prefixed reader.
#include "itch/itch.hpp"
#include "itch/raw_itch.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

namespace nsq::itch {
namespace {

TEST(MessageSize, MatchesDecodedSubsetKSizes) {
  EXPECT_EQ(message_size('S'), SystemEvent::kSize);
  EXPECT_EQ(message_size('R'), StockDirectory::kSize);
  EXPECT_EQ(message_size('H'), TradingAction::kSize);
  EXPECT_EQ(message_size('A'), AddOrder::kSize);
  EXPECT_EQ(message_size('F'), AddOrderMpid::kSize);
  EXPECT_EQ(message_size('E'), OrderExecuted::kSize);
  EXPECT_EQ(message_size('C'), OrderExecutedWithPrice::kSize);
  EXPECT_EQ(message_size('X'), OrderCancel::kSize);
  EXPECT_EQ(message_size('D'), OrderDelete::kSize);
  EXPECT_EQ(message_size('U'), OrderReplace::kSize);
  EXPECT_EQ(message_size('P'), Trade::kSize);
}

TEST(MessageSize, CoversFullItch50SpecAndRejectsUnknown) {
  // Types outside our decoded subset, sizes per NASDAQ TotalView-ITCH 5.0.
  EXPECT_EQ(message_size('Y'), 20u);  // Reg SHO restriction
  EXPECT_EQ(message_size('L'), 26u);  // market participant position
  EXPECT_EQ(message_size('V'), 35u);  // MWCB decline levels
  EXPECT_EQ(message_size('W'), 12u);  // MWCB status
  EXPECT_EQ(message_size('K'), 28u);  // IPO quoting period
  EXPECT_EQ(message_size('J'), 35u);  // LULD auction collar
  EXPECT_EQ(message_size('h'), 21u);  // operational halt
  EXPECT_EQ(message_size('Q'), 40u);  // cross trade
  EXPECT_EQ(message_size('B'), 19u);  // broken trade
  EXPECT_EQ(message_size('I'), 50u);  // NOII
  EXPECT_EQ(message_size('N'), 20u);  // RPII
  EXPECT_EQ(message_size('O'), 48u);  // direct listing w/ capital raise
  EXPECT_EQ(message_size('?'), 0u);
  EXPECT_EQ(message_size('z'), 0u);
}

std::FILE* tmpfile_with(const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::tmpfile();
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::rewind(f);
  return f;
}

std::vector<std::uint8_t> encoded_add(std::uint64_t ref) {
  AddOrder m{};
  m.order_ref = ref;
  m.side = Side::Buy;
  m.shares = 100;
  m.stock = make_symbol("AAPL");
  m.price = make_price(100, 0);
  std::vector<std::uint8_t> buf(AddOrder::kSize);
  encode(m, buf.data());
  return buf;
}

TEST(RawItchReader, ReadsFramedMessagesAndStopsAtEof) {
  std::vector<std::uint8_t> stream;
  const auto a = encoded_add(1), b = encoded_add(2);
  append_raw(stream, a.data(), a.size());
  append_raw(stream, b.data(), b.size());

  std::FILE* f = tmpfile_with(stream);
  RawItchReader r(f);
  const auto m1 = r.next();
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(m1->size(), AddOrder::kSize);
  EXPECT_EQ((*m1)[0], 'A');
  ASSERT_TRUE(r.next().has_value());
  EXPECT_FALSE(r.next().has_value());
  EXPECT_TRUE(r.error().empty());
  std::fclose(f);
}

TEST(RawItchReader, FramesUnknownButSizedTypesForSkipping) {
  // A NOII 'I' (50 bytes, outside the decoded subset) between two adds:
  // the reader must frame it; decode() will simply return nullopt for it.
  std::vector<std::uint8_t> stream;
  const auto a = encoded_add(1);
  append_raw(stream, a.data(), a.size());
  std::vector<std::uint8_t> noii(50, 0);
  noii[0] = 'I';
  append_raw(stream, noii.data(), noii.size());
  append_raw(stream, a.data(), a.size());

  std::FILE* f = tmpfile_with(stream);
  RawItchReader r(f);
  int frames = 0, decoded = 0;
  while (const auto m = r.next()) {
    ++frames;
    if (decode(m->data(), m->size())) ++decoded;
  }
  EXPECT_EQ(frames, 3);
  EXPECT_EQ(decoded, 2);
  EXPECT_TRUE(r.error().empty());
  std::fclose(f);
}

TEST(RawItchReader, LengthContradictingKnownTypeIsError) {
  std::vector<std::uint8_t> stream;
  std::vector<std::uint8_t> bad(35, 0);  // AddOrder must be 36
  bad[0] = 'A';
  append_raw(stream, bad.data(), bad.size());

  std::FILE* f = tmpfile_with(stream);
  RawItchReader r(f);
  EXPECT_FALSE(r.next().has_value());
  EXPECT_FALSE(r.error().empty());
  EXPECT_NE(r.error().find("offset"), std::string::npos);
  std::fclose(f);
}

TEST(RawItchReader, TruncatedTailToleratedSilently) {
  std::vector<std::uint8_t> stream;
  const auto a = encoded_add(1);
  append_raw(stream, a.data(), a.size());
  append_raw(stream, a.data(), a.size());
  stream.resize(stream.size() - 10);  // cut into the second message

  std::FILE* f = tmpfile_with(stream);
  RawItchReader r(f);
  EXPECT_TRUE(r.next().has_value());
  EXPECT_FALSE(r.next().has_value());
  EXPECT_TRUE(r.error().empty());
  std::fclose(f);
}

}  // namespace
}  // namespace nsq::itch

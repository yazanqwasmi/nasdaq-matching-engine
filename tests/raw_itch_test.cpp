// Raw-ITCH framing: full message-size table and length-prefixed reader.
#include "itch/itch.hpp"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace nsq::itch

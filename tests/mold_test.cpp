// MoldUDP64 packetizer/depacketizer tests: header layout, MTU-aware
// batching, sequence accounting, heartbeats, end-of-session, gap detection.
#include "mold/mold.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace nsq::mold {
namespace {

Session test_session() { return make_session("TESTSESS"); }

std::vector<std::uint8_t> msg(std::string s) {
  return {s.begin(), s.end()};
}

TEST(MoldHeader, GoldenLayout) {
  Packetizer pk(test_session(), /*mtu=*/1400);
  EXPECT_EQ(pk.next_seq(), 1u);

  const auto m = msg("hello");
  pk.add_message(m.data(), m.size());
  pk.flush();
  auto packets = pk.take_packets();
  ASSERT_EQ(packets.size(), 1u);
  const auto& p = packets[0];

  // session(10) + seq(8) + count(2) + [len(2) + payload]
  ASSERT_EQ(p.size(), kHeaderSize + 2 + 5);
  EXPECT_EQ(p[0], 'T');
  EXPECT_EQ(p[9], ' ');   // session space-padded to 10
  EXPECT_EQ(p[17], 1);    // seq BE64 at 10..17 == 1
  EXPECT_EQ(p[18], 0);    // count BE16 at 18..19 == 1
  EXPECT_EQ(p[19], 1);
  EXPECT_EQ(p[20], 0);    // message length BE16 == 5
  EXPECT_EQ(p[21], 5);
  EXPECT_EQ(p[22], 'h');

  EXPECT_EQ(pk.next_seq(), 2u);
}

TEST(MoldPacketizer, BatchesMessagesUnderMtu) {
  Packetizer pk(test_session(), 1400);
  const auto a = msg("aaa"), b = msg("bbbb");
  pk.add_message(a.data(), a.size());
  pk.add_message(b.data(), b.size());
  pk.flush();
  const auto packets = pk.take_packets();
  ASSERT_EQ(packets.size(), 1u);

  const auto parsed = parse(packets[0].data(), packets[0].size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->seq, 1u);
  ASSERT_EQ(parsed->messages.size(), 2u);
  EXPECT_EQ(std::string(parsed->messages[0].begin(), parsed->messages[0].end()),
            "aaa");
  EXPECT_EQ(std::string(parsed->messages[1].begin(), parsed->messages[1].end()),
            "bbbb");
  EXPECT_EQ(pk.next_seq(), 3u);  // two messages consumed seq 1 and 2
}

TEST(MoldPacketizer, SplitsAtMtu) {
  // MTU fits header + one 100-byte message (+2 length) but not two.
  Packetizer pk(test_session(), kHeaderSize + 102 + 50);
  const std::vector<std::uint8_t> big(100, 0xEE);
  pk.add_message(big.data(), big.size());
  pk.add_message(big.data(), big.size());
  pk.flush();
  const auto packets = pk.take_packets();
  ASSERT_EQ(packets.size(), 2u);
  const auto p0 = parse(packets[0].data(), packets[0].size());
  const auto p1 = parse(packets[1].data(), packets[1].size());
  ASSERT_TRUE(p0 && p1);
  EXPECT_EQ(p0->seq, 1u);
  EXPECT_EQ(p1->seq, 2u);
  EXPECT_EQ(p0->messages.size(), 1u);
  EXPECT_EQ(p1->messages.size(), 1u);
}

TEST(MoldPacketizer, HeartbeatCarriesNextSeqAndZeroCount) {
  Packetizer pk(test_session(), 1400);
  const auto m = msg("x");
  pk.add_message(m.data(), m.size());
  pk.flush();
  pk.take_packets();

  const auto hb = pk.heartbeat();
  const auto parsed = parse(hb.data(), hb.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->seq, 2u);
  EXPECT_TRUE(parsed->messages.empty());
  EXPECT_FALSE(parsed->end_of_session);
}

TEST(MoldPacketizer, EndOfSession) {
  Packetizer pk(test_session(), 1400);
  const auto eos = pk.end_of_session();
  const auto parsed = parse(eos.data(), eos.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->end_of_session);
  EXPECT_TRUE(parsed->messages.empty());
}

TEST(MoldParse, RejectsTruncatedPackets) {
  Packetizer pk(test_session(), 1400);
  const auto m = msg("hello");
  pk.add_message(m.data(), m.size());
  pk.flush();
  const auto p = pk.take_packets()[0];
  EXPECT_FALSE(parse(p.data(), kHeaderSize - 1).has_value());
  EXPECT_FALSE(parse(p.data(), p.size() - 1).has_value());
}

TEST(MoldGapDetector, DetectsGapAndTracksExpected) {
  GapDetector gd;
  EXPECT_FALSE(gd.on_packet(1, 2));  // seq 1, 2 messages -> expect 3 next
  EXPECT_EQ(gd.expected(), 3u);
  EXPECT_FALSE(gd.on_packet(3, 1));
  EXPECT_EQ(gd.expected(), 4u);

  EXPECT_TRUE(gd.on_packet(6, 1));  // missed 4 and 5
  EXPECT_TRUE(gd.had_gap());
  EXPECT_EQ(gd.expected(), 7u);  // resynced past the gap
}

TEST(MoldGapDetector, HeartbeatsDoNotAdvanceSeq) {
  GapDetector gd;
  EXPECT_FALSE(gd.on_packet(1, 1));
  EXPECT_FALSE(gd.on_packet(2, 0));  // heartbeat carrying next seq
  EXPECT_EQ(gd.expected(), 2u);
  EXPECT_FALSE(gd.on_packet(2, 1));
}

}  // namespace
}  // namespace nsq::mold

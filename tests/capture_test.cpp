// Capture-file tests: record round-trip, feed tee, and reconstruction of
// the identical book from a captured session.
#include "client/itch_book.hpp"
#include "engine/engine.hpp"
#include "feed/capture.hpp"
#include "feed/feed.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <random>

namespace nsq {
namespace {

std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() /
          (std::string("nsqcap_") + name + std::to_string(::getpid())))
      .string();
}

TEST(Capture, WriteReadRoundTrip) {
  const std::string path = temp_path("rt");
  {
    feed::CaptureWriter w(path);
    ASSERT_TRUE(w.ok());
    const std::vector<std::uint8_t> a{1, 2, 3}, b(300, 0xAB);
    w.write(1000, a.data(), a.size());
    w.write(2000, b.data(), b.size());
  }
  feed::CaptureReader r(path);
  ASSERT_TRUE(r.ok());
  auto rec1 = r.next();
  ASSERT_TRUE(rec1.has_value());
  EXPECT_EQ(rec1->ts_ns, 1000u);
  EXPECT_EQ(rec1->bytes.size(), 3u);
  auto rec2 = r.next();
  ASSERT_TRUE(rec2.has_value());
  EXPECT_EQ(rec2->ts_ns, 2000u);
  EXPECT_EQ(rec2->bytes.size(), 300u);
  EXPECT_EQ(rec2->bytes[0], 0xAB);
  EXPECT_FALSE(r.next().has_value());
  std::filesystem::remove(path);
}

TEST(Capture, RejectsBadMagicAndTruncation) {
  const std::string path = temp_path("bad");
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite("NOPE", 1, 4, f);
    std::fclose(f);
  }
  feed::CaptureReader r(path);
  EXPECT_FALSE(r.ok());
  std::filesystem::remove(path);

  const std::string path2 = temp_path("trunc");
  {
    feed::CaptureWriter w(path2);
    const std::vector<std::uint8_t> a(100, 1);
    w.write(1, a.data(), a.size());
  }
  // Truncate mid-record.
  std::filesystem::resize_file(path2, 8 + 5);
  feed::CaptureReader r2(path2);
  ASSERT_TRUE(r2.ok());
  EXPECT_FALSE(r2.next().has_value());  // stops cleanly, no crash
  std::filesystem::remove(path2);
}

TEST(Capture, CapturedSessionReconstructsIdenticalBook) {
  const std::string path = temp_path("sess");

  engine::CommandChannel in;
  engine::MpscQueue<engine::ClientResponse> to_gateway;
  engine::MpscQueue<engine::MarketEvent> to_feed;
  engine::Engine eng{in, to_gateway, to_feed};

  feed::FeedConfig cfg;
  cfg.dest_ip = "127.0.0.1";
  cfg.dest_port = 1;  // nobody listens; capture is what we verify
  cfg.capture_path = path;
  {
    feed::FeedPublisher pub{to_feed, cfg};
    pub.start();

    std::mt19937_64 rng(11);
    std::uniform_int_distribution<Price> tick(-20, 20);
    std::uniform_int_distribution<Qty> qty(1, 500);
    for (int i = 0; i < 400; ++i) {
      ouch::EnterOrder m{};
      m.token = ouch::make_token("T" + std::to_string(i));
      m.side = (rng() & 1) ? Side::Buy : Side::Sell;
      m.shares = qty(rng);
      m.stock = make_symbol("AAPL");
      m.price = make_price(100, 0) + tick(rng) * make_price(0, 100);
      m.tif = 99999;
      eng.process({1, m});
    }
    to_gateway.drain();
    pub.stop();
  }

  // Rebuild the book purely from the capture file.
  client::ItchBookBuilder builder;
  feed::CaptureReader r(path);
  ASSERT_TRUE(r.ok());
  bool eos = false;
  while (auto rec = r.next())
    eos = builder.on_mold_packet(rec->bytes.data(), rec->bytes.size()) || eos;
  EXPECT_TRUE(eos);
  EXPECT_FALSE(builder.had_gap());
  EXPECT_EQ(eng.book_snapshot(make_symbol("AAPL")),
            builder.snapshot(make_symbol("AAPL")));
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace nsq

// Tests for the log-linear latency histogram and the lock-free SPSC ring.
#include "common/histogram.hpp"
#include "common/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace nsq {
namespace {

TEST(Histogram, PercentilesWithinBucketResolution) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 10'000; ++v) h.record(v);
  EXPECT_EQ(h.count(), 10'000u);
  EXPECT_EQ(h.max(), 10'000u);
  // Log-linear buckets have ~6.25% resolution; allow 10%.
  EXPECT_NEAR(static_cast<double>(h.percentile(50.0)), 5000.0, 500.0);
  EXPECT_NEAR(static_cast<double>(h.percentile(99.0)), 9900.0, 990.0);
  EXPECT_GE(h.percentile(100.0), 10'000u);
}

TEST(Histogram, EmptyAndSingle) {
  LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.percentile(50.0), 0u);
  h.record(42);
  EXPECT_GE(h.percentile(50.0), 42u);
  EXPECT_LE(h.percentile(50.0), 45u);
}

TEST(SpscRing, PushPopSingleThread) {
  SpscRing<int, 8> ring;
  EXPECT_FALSE(ring.try_pop().has_value());
  for (int i = 0; i < 7; ++i) EXPECT_TRUE(ring.try_push(i));  // cap N-1
  EXPECT_FALSE(ring.try_push(7));                             // full
  for (int i = 0; i < 7; ++i) {
    const auto v = ring.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }
  EXPECT_FALSE(ring.try_pop().has_value());
}

TEST(SpscRing, TwoThreadStressPreservesOrder) {
  SpscRing<std::uint64_t, 1024> ring;
  constexpr std::uint64_t kItems = 2'000'000;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i)
      while (!ring.try_push(i)) {
      }
  });

  std::uint64_t expected = 0;
  while (expected < kItems) {
    if (const auto v = ring.try_pop()) {
      ASSERT_EQ(*v, expected);
      ++expected;
    }
  }
  producer.join();
  EXPECT_FALSE(ring.try_pop().has_value());
}

}  // namespace
}  // namespace nsq

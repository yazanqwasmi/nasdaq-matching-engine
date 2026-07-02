// Randomized invariant test: pump thousands of random add/cancel/replace
// operations through the reference book, checking invariants after every op.
// Also prints a rough orders/sec number as an accidental-O(n^2) canary
// (not a performance goal).
#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace nsq {
namespace {

struct CountingListener : BookListener {
  Qty executed_qty = 0;
  std::size_t added = 0, canceled = 0, replaced = 0;
  std::vector<OrderId> live;  // ids the listener believes are resting

  void on_added(const AddedEvent& e) override {
    ++added;
    live.push_back(e.id);
  }
  void on_executed(const ExecutedEvent& e) override { executed_qty += e.qty; }
  void on_canceled(const CanceledEvent&) override { ++canceled; }
  void on_replaced(const ReplacedEvent&) override { ++replaced; }
};

TEST(BookRandom, InvariantsHoldOverTenThousandRandomOps) {
  CountingListener events;
  OrderBook book{events};
  std::mt19937_64 rng(0xC0FFEE);

  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<Price> tick_dist(-50, 50);
  std::uniform_int_distribution<Qty> qty_dist(1, 1000);
  const Price mid = make_price(100, 0);
  const Price tick = make_price(0, 100);  // one cent

  OrderId next_id = 1;
  std::vector<OrderId> live;

  for (int i = 0; i < 10'000; ++i) {
    const int op = op_dist(rng);
    if (op < 60 || live.empty()) {
      // 60%: new order around the mid.
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price px = mid + tick_dist(rng) * tick;
      const OrderId id = next_id++;
      const auto r = book.add(id, side, px, qty_dist(rng));
      ASSERT_EQ(r.status, AddStatus::Ok);
      if (r.resting > 0) live.push_back(id);
    } else if (op < 85) {
      // 25%: cancel a random live order (may already be gone via execution).
      const std::size_t k = rng() % live.size();
      book.cancel(live[k]);
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    } else {
      // 15%: replace a random live order (may fail if already executed).
      const std::size_t k = rng() % live.size();
      const OrderId new_id = next_id++;
      const Price px = mid + tick_dist(rng) * tick;
      if (book.replace(live[k], new_id, px, qty_dist(rng))) {
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        if (book.contains(new_id)) live.push_back(new_id);
      } else {
        // Replace failed because the order was fully executed earlier.
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      }
    }
    ASSERT_NO_FATAL_FAILURE(book.check_invariants()) << "after op " << i;
  }

  // Sanity: the run actually exercised all paths.
  EXPECT_GT(events.added, 100u);
  EXPECT_GT(events.executed_qty, 0u);
  EXPECT_GT(events.canceled, 0u);
  EXPECT_GT(events.replaced, 0u);
}

TEST(BookRandom, ThroughputCanary) {
  CountingListener events;
  OrderBook book{events};
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<Price> tick_dist(-100, 100);
  std::uniform_int_distribution<Qty> qty_dist(1, 500);
  const Price mid = make_price(100, 0);
  const Price tick = make_price(0, 100);

  constexpr int kOps = 200'000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kOps; ++i) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    book.add(static_cast<OrderId>(i + 1), side, mid + tick_dist(rng) * tick,
             qty_dist(rng));
    if ((i & 7) == 0)
      book.cancel(rng() % (static_cast<OrderId>(i) + 1) + 1);
  }
  const auto dt = std::chrono::steady_clock::now() - t0;
  const double secs = std::chrono::duration<double>(dt).count();
  std::printf("[canary] %d ops in %.3fs => %.0f ops/sec\n", kOps, secs,
              kOps / secs);
  // Generous bound: only trips on accidental O(n^2) behavior.
  EXPECT_LT(secs, 10.0);
}

}  // namespace
}  // namespace nsq

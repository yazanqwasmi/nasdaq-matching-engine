// FastBook correctness: directed edge cases plus the differential fuzz —
// the same random command stream drives FastBook and the reference
// OrderBook; event streams must be byte-identical and snapshots equal.
#include "book/fast_book.hpp"
#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <random>
#include <variant>
#include <vector>

namespace nsq {
namespace {

using AnyEvent =
    std::variant<AddedEvent, ExecutedEvent, CanceledEvent, ReplacedEvent>;

struct Recorder : BookListener {
  std::vector<AnyEvent> events;
  void on_added(const AddedEvent& e) override { events.push_back(e); }
  void on_executed(const ExecutedEvent& e) override { events.push_back(e); }
  void on_canceled(const CanceledEvent& e) override { events.push_back(e); }
  void on_replaced(const ReplacedEvent& e) override { events.push_back(e); }
};

TEST(FastBook, BasicCrossAndFifo) {
  Recorder rec;
  FastBook book{rec};
  book.add(1, Side::Sell, make_price(100, 0), 100);
  book.add(2, Side::Sell, make_price(100, 0), 100);
  const auto r = book.add(3, Side::Buy, make_price(100, 5000), 150);
  EXPECT_EQ(r.filled, 150u);
  EXPECT_EQ(r.resting, 0u);
  ASSERT_EQ(rec.events.size(), 4u);  // A, A, E, E
  const auto* e1 = std::get_if<ExecutedEvent>(&rec.events[2]);
  const auto* e2 = std::get_if<ExecutedEvent>(&rec.events[3]);
  ASSERT_NE(e1, nullptr);
  ASSERT_NE(e2, nullptr);
  EXPECT_EQ(e1->resting_id, 1u);
  EXPECT_EQ(e1->qty, 100u);
  EXPECT_EQ(e2->resting_id, 2u);
  EXPECT_EQ(e2->qty, 50u);
  EXPECT_EQ(*book.best_ask(), make_price(100, 0));
  book.check_invariants();
}

TEST(FastBook, OutOfBandAndOddTickPricesUseFallback) {
  Recorder rec;
  FastBook::Config cfg;
  cfg.tick = make_price(0, 100);        // 1 cent
  cfg.band_half_ticks = 100;            // tiny band: +/- $1 around first px
  FastBook book{rec, cfg};

  book.add(1, Side::Buy, make_price(100, 0), 100);    // sets band center
  book.add(2, Side::Buy, make_price(500, 0), 100);    // far out of band
  book.add(3, Side::Sell, make_price(600, 0), 100);   // far out of band
  book.add(4, Side::Buy, make_price(100, 137), 50);   // off-tick price
  book.check_invariants();

  EXPECT_EQ(*book.best_bid(), make_price(500, 0));
  EXPECT_EQ(*book.best_ask(), make_price(600, 0));

  // A sell at 400 crosses only the 500 bid (100 shares, executed at the
  // resting price); the remaining 50 rest at 400 as the new best ask.
  const auto r = book.add(5, Side::Sell, make_price(400, 0), 150);
  EXPECT_EQ(r.filled, 100u);
  EXPECT_EQ(r.resting, 50u);
  EXPECT_EQ(*book.best_bid(), make_price(100, 137));
  EXPECT_EQ(*book.best_ask(), make_price(400, 0));
  book.check_invariants();

  // Cancel the odd-tick order.
  EXPECT_TRUE(book.cancel(4));
  EXPECT_EQ(*book.best_bid(), make_price(100, 0));
  book.check_invariants();
}

// The oracle test: identical inputs, identical outputs, half a million ops.
TEST(FastBook, DifferentialFuzzAgainstReferenceBook) {
  Recorder ref_rec, fast_rec;
  OrderBook ref{ref_rec};
  FastBook fast{fast_rec};

  std::mt19937_64 rng(0xFA57B00C);
  std::uniform_int_distribution<int> op(0, 99);
  std::uniform_int_distribution<Price> tick_off(-60, 60);
  std::uniform_int_distribution<Qty> qty(1, 800);
  const Price mid = make_price(100, 0);
  const Price cent = make_price(0, 100);

  OrderId next_id = 1;
  std::vector<OrderId> live;

  const auto random_price = [&]() -> Price {
    const int roll = op(rng);
    if (roll < 90) return mid + tick_off(rng) * cent;      // in band
    if (roll < 95) return mid + tick_off(rng) * cent + 37; // off-tick
    return mid * (roll % 2 ? 5 : 1) / (roll % 2 ? 1 : 5) + tick_off(rng) * cent;  // far away
  };

  for (int i = 0; i < 500'000; ++i) {
    const int roll = op(rng);
    if (roll < 55 || live.empty()) {
      const OrderId id = next_id++;
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price px = random_price();
      const Qty q = qty(rng);
      const auto r1 = ref.add(id, side, px, q);
      const auto r2 = fast.add(id, side, px, q);
      ASSERT_EQ(r1.status, r2.status) << "op " << i;
      ASSERT_EQ(r1.filled, r2.filled) << "op " << i;
      ASSERT_EQ(r1.resting, r2.resting) << "op " << i;
      if (r1.status == AddStatus::Ok && r1.resting > 0) live.push_back(id);
    } else if (roll < 80) {
      const std::size_t k = rng() % live.size();
      const Qty keep = (roll % 4 == 0) ? qty(rng) / 4 : 0;
      ASSERT_EQ(ref.cancel(live[k], keep), fast.cancel(live[k], keep))
          << "op " << i;
      if (keep == 0) live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    } else {
      const std::size_t k = rng() % live.size();
      const OrderId new_id = next_id++;
      const Price px = random_price();
      const Qty q = qty(rng);
      const bool ok1 = ref.replace(live[k], new_id, px, q);
      const bool ok2 = fast.replace(live[k], new_id, px, q);
      ASSERT_EQ(ok1, ok2) << "op " << i;
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      if (ok1 && ref.contains(new_id)) live.push_back(new_id);
    }

    ASSERT_EQ(ref_rec.events.size(), fast_rec.events.size()) << "op " << i;
    if (i % 10'000 == 0) {
      ASSERT_EQ(ref_rec.events, fast_rec.events) << "op " << i;
      ASSERT_EQ(ref.snapshot(), fast.snapshot()) << "op " << i;
      ASSERT_NO_THROW(fast.check_invariants()) << "op " << i;
    }
  }
  ASSERT_EQ(ref_rec.events, fast_rec.events);
  ASSERT_EQ(ref.snapshot(), fast.snapshot());
  fast.check_invariants();
}

}  // namespace
}  // namespace nsq

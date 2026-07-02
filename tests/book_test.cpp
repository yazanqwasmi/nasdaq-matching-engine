#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

namespace nsq {
namespace {

// Records every event the book emits, in order.
struct RecordingListener : BookListener {
  std::vector<AddedEvent> added;
  std::vector<ExecutedEvent> executed;
  std::vector<CanceledEvent> canceled;
  std::vector<ReplacedEvent> replaced;

  void on_added(const AddedEvent& e) override { added.push_back(e); }
  void on_executed(const ExecutedEvent& e) override { executed.push_back(e); }
  void on_canceled(const CanceledEvent& e) override { canceled.push_back(e); }
  void on_replaced(const ReplacedEvent& e) override { replaced.push_back(e); }
};

class BookTest : public ::testing::Test {
 protected:
  RecordingListener events;
  OrderBook book{events};

  void expect_invariants() { ASSERT_NO_FATAL_FAILURE(book.check_invariants()); }
};

TEST_F(BookTest, RestingOrderEmitsAddedAndSetsBestBid) {
  const auto r = book.add(1, Side::Buy, make_price(100, 0), 500);
  EXPECT_EQ(r.status, AddStatus::Ok);
  EXPECT_EQ(r.filled, 0u);
  EXPECT_EQ(r.resting, 500u);

  ASSERT_EQ(events.added.size(), 1u);
  EXPECT_EQ(events.added[0].id, 1u);
  EXPECT_EQ(events.added[0].qty, 500u);

  ASSERT_TRUE(book.best_bid().has_value());
  EXPECT_EQ(*book.best_bid(), make_price(100, 0));
  EXPECT_FALSE(book.best_ask().has_value());
  expect_invariants();
}

TEST_F(BookTest, NonCrossingOrdersRestOnBothSides) {
  book.add(1, Side::Buy, make_price(99, 0), 100);
  book.add(2, Side::Sell, make_price(101, 0), 100);
  EXPECT_EQ(*book.best_bid(), make_price(99, 0));
  EXPECT_EQ(*book.best_ask(), make_price(101, 0));
  EXPECT_TRUE(events.executed.empty());
  expect_invariants();
}

TEST_F(BookTest, CrossingOrderExecutesAtRestingPrice) {
  book.add(1, Side::Sell, make_price(100, 0), 300);
  // Aggressive buy willing to pay 101 executes at the resting 100.
  const auto r = book.add(2, Side::Buy, make_price(101, 0), 300);
  EXPECT_EQ(r.filled, 300u);
  EXPECT_EQ(r.resting, 0u);

  ASSERT_EQ(events.executed.size(), 1u);
  EXPECT_EQ(events.executed[0].resting_id, 1u);
  EXPECT_EQ(events.executed[0].aggressor_id, 2u);
  EXPECT_EQ(events.executed[0].price, make_price(100, 0));
  EXPECT_EQ(events.executed[0].qty, 300u);
  EXPECT_EQ(events.executed[0].aggressor_side, Side::Buy);

  // Fully executed aggressor never rests: exactly one Added (the sell).
  EXPECT_EQ(events.added.size(), 1u);
  EXPECT_FALSE(book.best_ask().has_value());
  EXPECT_FALSE(book.best_bid().has_value());
  expect_invariants();
}

TEST_F(BookTest, AggressorWalksBookAndRemainderRests) {
  book.add(1, Side::Sell, make_price(100, 0), 100);
  book.add(2, Side::Sell, make_price(100, 5000), 100);
  book.add(3, Side::Sell, make_price(101, 0), 100);

  const auto r = book.add(4, Side::Buy, make_price(100, 5000), 350);
  EXPECT_EQ(r.filled, 200u);   // fills both levels <= 100.50
  EXPECT_EQ(r.resting, 150u);  // remainder rests at 100.50

  ASSERT_EQ(events.executed.size(), 2u);
  EXPECT_EQ(events.executed[0].resting_id, 1u);
  EXPECT_EQ(events.executed[0].price, make_price(100, 0));
  EXPECT_EQ(events.executed[1].resting_id, 2u);
  EXPECT_EQ(events.executed[1].price, make_price(100, 5000));

  EXPECT_EQ(*book.best_bid(), make_price(100, 5000));
  EXPECT_EQ(*book.best_ask(), make_price(101, 0));
  expect_invariants();
}

TEST_F(BookTest, FifoWithinPriceLevel) {
  book.add(1, Side::Sell, make_price(100, 0), 100);
  book.add(2, Side::Sell, make_price(100, 0), 100);
  book.add(3, Side::Buy, make_price(100, 0), 150);

  ASSERT_EQ(events.executed.size(), 2u);
  EXPECT_EQ(events.executed[0].resting_id, 1u);  // first in, first filled
  EXPECT_EQ(events.executed[0].qty, 100u);
  EXPECT_EQ(events.executed[1].resting_id, 2u);
  EXPECT_EQ(events.executed[1].qty, 50u);
  expect_invariants();
}

TEST_F(BookTest, MatchIdsAreUniqueAndIncreasing) {
  book.add(1, Side::Sell, make_price(100, 0), 100);
  book.add(2, Side::Sell, make_price(100, 0), 100);
  book.add(3, Side::Buy, make_price(100, 0), 200);
  ASSERT_EQ(events.executed.size(), 2u);
  EXPECT_LT(events.executed[0].match_id, events.executed[1].match_id);
}

TEST_F(BookTest, FullCancelRemovesOrder) {
  book.add(1, Side::Buy, make_price(100, 0), 500);
  EXPECT_TRUE(book.cancel(1));

  ASSERT_EQ(events.canceled.size(), 1u);
  EXPECT_EQ(events.canceled[0].id, 1u);
  EXPECT_EQ(events.canceled[0].canceled_qty, 500u);
  EXPECT_TRUE(events.canceled[0].removed);
  EXPECT_FALSE(book.best_bid().has_value());
  expect_invariants();
}

TEST_F(BookTest, PartialCancelReducesOpenQty) {
  // OUCH 4.2 cancel semantics: reduce open shares to the requested amount.
  book.add(1, Side::Buy, make_price(100, 0), 500);
  EXPECT_TRUE(book.cancel(1, 200));  // keep 200 open

  ASSERT_EQ(events.canceled.size(), 1u);
  EXPECT_EQ(events.canceled[0].canceled_qty, 300u);
  EXPECT_FALSE(events.canceled[0].removed);

  // Time priority is retained on reduce: order 1 still fills first.
  book.add(2, Side::Buy, make_price(100, 0), 100);
  book.add(3, Side::Sell, make_price(100, 0), 250);
  ASSERT_EQ(events.executed.size(), 2u);
  EXPECT_EQ(events.executed[0].resting_id, 1u);
  EXPECT_EQ(events.executed[0].qty, 200u);
  EXPECT_EQ(events.executed[1].resting_id, 2u);
  EXPECT_EQ(events.executed[1].qty, 50u);
  expect_invariants();
}

TEST_F(BookTest, CancelUnknownOrderFails) {
  EXPECT_FALSE(book.cancel(42));
  EXPECT_TRUE(events.canceled.empty());
}

TEST_F(BookTest, CancelKeepingMoreThanOpenIsNoop) {
  book.add(1, Side::Buy, make_price(100, 0), 100);
  EXPECT_FALSE(book.cancel(1, 200));
  EXPECT_TRUE(events.canceled.empty());
  expect_invariants();
}

TEST_F(BookTest, ReplaceLosesTimePriority) {
  book.add(1, Side::Buy, make_price(100, 0), 100);
  book.add(2, Side::Buy, make_price(100, 0), 100);

  // Order 1 was first; replacing it moves it behind order 2.
  EXPECT_TRUE(book.replace(1, 10, make_price(100, 0), 100));
  ASSERT_EQ(events.replaced.size(), 1u);
  EXPECT_EQ(events.replaced[0].old_id, 1u);
  EXPECT_EQ(events.replaced[0].new_id, 10u);

  book.add(3, Side::Sell, make_price(100, 0), 150);
  ASSERT_EQ(events.executed.size(), 2u);
  EXPECT_EQ(events.executed[0].resting_id, 2u);
  EXPECT_EQ(events.executed[1].resting_id, 10u);
  EXPECT_EQ(events.executed[1].qty, 50u);
  expect_invariants();
}

TEST_F(BookTest, ReplaceToCrossingPriceCancelsThenExecutes) {
  book.add(1, Side::Sell, make_price(101, 0), 100);
  book.add(2, Side::Buy, make_price(99, 0), 100);

  // Replace the buy up through the ask: emitted as cancel + execution,
  // not as an ITCH-style replace (documented subset behavior).
  EXPECT_TRUE(book.replace(2, 20, make_price(101, 0), 100));
  EXPECT_TRUE(events.replaced.empty());
  ASSERT_EQ(events.canceled.size(), 1u);
  EXPECT_EQ(events.canceled[0].id, 2u);
  ASSERT_EQ(events.executed.size(), 1u);
  EXPECT_EQ(events.executed[0].resting_id, 1u);
  EXPECT_EQ(events.executed[0].aggressor_id, 20u);
  expect_invariants();
}

TEST_F(BookTest, ReplaceUnknownOrderFails) {
  EXPECT_FALSE(book.replace(42, 43, make_price(100, 0), 100));
}

TEST_F(BookTest, ReplaceToDuplicateIdFails) {
  book.add(1, Side::Buy, make_price(100, 0), 100);
  book.add(2, Side::Buy, make_price(99, 0), 100);
  EXPECT_FALSE(book.replace(1, 2, make_price(100, 0), 100));
  expect_invariants();
}

TEST_F(BookTest, DuplicateOrderIdRejected) {
  book.add(1, Side::Buy, make_price(100, 0), 100);
  const auto r = book.add(1, Side::Sell, make_price(200, 0), 100);
  EXPECT_EQ(r.status, AddStatus::DuplicateId);
  EXPECT_EQ(events.added.size(), 1u);
  expect_invariants();
}

TEST_F(BookTest, InvalidPriceAndQtyRejected) {
  EXPECT_EQ(book.add(1, Side::Buy, 0, 100).status, AddStatus::BadPrice);
  EXPECT_EQ(book.add(2, Side::Buy, -make_price(1, 0), 100).status, AddStatus::BadPrice);
  EXPECT_EQ(book.add(3, Side::Buy, make_price(100, 0), 0).status, AddStatus::BadQty);
  EXPECT_TRUE(events.added.empty());
}

TEST_F(BookTest, SnapshotReflectsBookState) {
  book.add(1, Side::Buy, make_price(99, 0), 100);
  book.add(2, Side::Buy, make_price(99, 0), 200);
  book.add(3, Side::Buy, make_price(98, 0), 300);
  book.add(4, Side::Sell, make_price(101, 0), 400);

  const auto snap = book.snapshot();
  ASSERT_EQ(snap.bids.size(), 2u);
  EXPECT_EQ(snap.bids[0].price, make_price(99, 0));  // best first
  ASSERT_EQ(snap.bids[0].orders.size(), 2u);
  EXPECT_EQ(snap.bids[0].orders[0].id, 1u);  // FIFO order
  EXPECT_EQ(snap.bids[0].orders[1].id, 2u);
  EXPECT_EQ(snap.bids[1].price, make_price(98, 0));
  ASSERT_EQ(snap.asks.size(), 1u);
  EXPECT_EQ(snap.asks[0].orders[0].qty, 400u);
}

TEST_F(BookTest, ExecutionConservation) {
  // Every execution reduces buy and sell open interest equally; total
  // filled qty reported to aggressors == total qty in executed events.
  book.add(1, Side::Sell, make_price(100, 0), 100);
  book.add(2, Side::Sell, make_price(100, 0), 100);
  const auto r = book.add(3, Side::Buy, make_price(100, 0), 500);
  Qty exec_total = 0;
  for (const auto& e : events.executed) exec_total += e.qty;
  EXPECT_EQ(exec_total, r.filled);
  EXPECT_EQ(r.filled + r.resting, 500u);
  expect_invariants();
}

}  // namespace
}  // namespace nsq

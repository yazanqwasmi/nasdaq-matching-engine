// Reference limit order book: price-time priority matching with heavy
// invariant checking. This is the canonical, auditable implementation and
// the oracle for differential-fuzzing any optimized book later.
//
// The book is per-symbol and protocol-agnostic: it knows nothing about
// OUCH/ITCH, clocks, or threads, and is fully deterministic. Events are
// emitted synchronously through BookListener during each call.
//
// Documented subset semantics:
//  - Limit orders only, executions at the resting order's price.
//  - cancel(id, keep) reduces open shares to `keep` (OUCH 4.2 style);
//    keep == 0 deletes. Reducing retains time priority.
//  - replace always loses time priority. A replace whose new price would
//    cross is emitted as cancel(old) + normal add lifecycle for the new id
//    (no ReplacedEvent), mirroring how it must appear on a market data feed.
#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nsq {

struct AddedEvent {
  OrderId id;
  Side side;
  Price price;
  Qty qty;  // resting (post-match) quantity
  bool operator==(const AddedEvent&) const = default;
};

struct ExecutedEvent {
  OrderId resting_id;
  OrderId aggressor_id;
  Side aggressor_side;
  Price price;  // execution price == resting order's price
  Qty qty;
  std::uint64_t match_id;
  bool operator==(const ExecutedEvent&) const = default;
};

struct CanceledEvent {
  OrderId id;
  Qty canceled_qty;
  bool removed;  // false for a partial reduction
  bool operator==(const CanceledEvent&) const = default;
};

struct ReplacedEvent {
  OrderId old_id;
  OrderId new_id;
  Side side;
  Price price;
  Qty qty;
  bool operator==(const ReplacedEvent&) const = default;
};

class BookListener {
 public:
  virtual ~BookListener() = default;
  virtual void on_added(const AddedEvent&) {}
  virtual void on_executed(const ExecutedEvent&) {}
  virtual void on_canceled(const CanceledEvent&) {}
  virtual void on_replaced(const ReplacedEvent&) {}
};

enum class AddStatus { Ok, DuplicateId, BadPrice, BadQty };

struct AddResult {
  AddStatus status;
  Qty filled;
  Qty resting;
};

struct SnapshotOrder {
  OrderId id;
  Qty qty;
  bool operator==(const SnapshotOrder&) const = default;
};

struct SnapshotLevel {
  Price price;
  std::vector<SnapshotOrder> orders;  // FIFO order
  bool operator==(const SnapshotLevel&) const = default;
};

struct BookSnapshot {
  std::vector<SnapshotLevel> bids;  // best (highest) first
  std::vector<SnapshotLevel> asks;  // best (lowest) first
  bool operator==(const BookSnapshot&) const = default;
};

class OrderBook {
 public:
  explicit OrderBook(BookListener& listener) : listener_(listener) {}

  AddResult add(OrderId id, Side side, Price price, Qty qty);
  bool cancel(OrderId id, Qty keep_qty = 0);
  bool replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty);

  bool contains(OrderId id) const { return index_.contains(id); }
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  BookSnapshot snapshot() const;

  // Throws std::logic_error on any violated invariant.
  void check_invariants() const;

 private:
  struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty open;
  };
  using LevelQueue = std::list<Order>;
  struct Level {
    Qty total = 0;
    LevelQueue orders;
  };
  using BidMap = std::map<Price, Level, std::greater<>>;
  using AskMap = std::map<Price, Level>;
  struct Handle {
    Side side;
    Price price;
    LevelQueue::iterator it;
  };

  template <typename OppositeMap>
  Qty match(OppositeMap& opposite, OrderId id, Side side, Price limit, Qty qty);
  void rest(OrderId id, Side side, Price price, Qty qty);
  void remove(std::unordered_map<OrderId, Handle>::iterator idx_it);
  bool would_cross(Side side, Price price) const;

  BookListener& listener_;
  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Handle> index_;
  std::uint64_t next_match_id_ = 1;
};

}  // namespace nsq

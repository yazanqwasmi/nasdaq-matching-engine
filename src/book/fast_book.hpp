// FastBook: cache-friendly limit order book with the exact semantics of the
// reference OrderBook (proven by differential fuzzing — see fastbook_test).
//
// Layout:
//  - Orders live in a pooled arena with intrusive prev/next links; freed
//    slots are recycled through a free list.
//  - Price levels for tick-aligned prices near the market live in a
//    contiguous ladder indexed by tick offset (O(1) lookup, cache-friendly
//    best-price scans). The band is centered on the first order's price.
//  - Out-of-band or off-tick prices fall back to ordered maps (rare path).
//  - Order-id lookup is an open-addressing FlatMap (no rehash at steady
//    state), so the in-band hot path performs zero heap allocations.
#pragma once

#include "book/flat_map.hpp"
#include "book/order_book.hpp"  // events, listener, AddResult, snapshot types

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nsq {

class FastBook {
 public:
  struct Config {
    Price tick = 100;               // $0.01
    int band_half_ticks = 20'000;   // +/- $200 around the first price
    std::size_t expected_orders = 1 << 16;
  };

  explicit FastBook(BookListener& listener);  // default Config
  FastBook(BookListener& listener, Config cfg);

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
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  struct FOrder {
    OrderId id;
    Price price;
    Qty open;
    std::uint32_t prev;
    std::uint32_t next;
    Side side;
  };
  struct Level {
    Qty total = 0;
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
  };

  // Ladder index for a price, or -1 when out of band / off tick.
  int tick_index(Price price) const;
  Price price_at(int idx) const {
    return base_ + static_cast<Price>(idx) * cfg_.tick;
  }
  Level& level_for(Side side, Price price);
  void erase_level_if_fallback(Side side, Price price, const Level& lvl);

  std::uint32_t alloc_order(OrderId id, Side side, Price price, Qty qty);
  void free_order(std::uint32_t idx);
  void unlink(Level& lvl, std::uint32_t idx);
  void rest(OrderId id, Side side, Price price, Qty qty);
  void remove_resting(std::uint32_t idx);

  // Best in-band tick with liquidity of `side`, or -1 (lazy cursor scan).
  int ladder_best_bid() const;
  int ladder_best_ask() const;
  Qty match(OrderId id, Side side, Price limit, Qty qty);
  bool would_cross(Side side, Price price) const;
  // Consumes FIFO orders at one level; returns qty filled there.
  Qty consume_level(Level& lvl, Price level_price, OrderId aggressor,
                    Side aggressor_side, Qty want);

  BookListener& listener_;
  Config cfg_;

  bool band_set_ = false;
  Price base_ = 0;  // price of ladder index 0
  std::vector<Level> ladder_;
  std::map<Price, Level, std::greater<>> fb_bids_;
  std::map<Price, Level> fb_asks_;

  mutable int bid_cursor_ = -1;                    // upper bound on best bid
  mutable int ask_cursor_ = 0;                     // lower bound on best ask
  std::vector<FOrder> pool_;
  std::vector<std::uint32_t> free_;
  FlatMap<OrderId, std::uint32_t> index_;
  std::uint64_t next_match_id_ = 1;
};

}  // namespace nsq

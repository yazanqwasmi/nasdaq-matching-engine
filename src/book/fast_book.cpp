#include "book/fast_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace nsq {

namespace {
bool crosses(Side aggressor_side, Price aggressor_limit, Price resting_price) {
  return aggressor_side == Side::Buy ? resting_price <= aggressor_limit
                                     : resting_price >= aggressor_limit;
}
}  // namespace

FastBook::FastBook(BookListener& listener) : FastBook(listener, Config{}) {}

FastBook::FastBook(BookListener& listener, Config cfg)
    : listener_(listener), cfg_(cfg), index_(cfg.expected_orders * 2) {
  pool_.reserve(cfg_.expected_orders);
  free_.reserve(cfg_.expected_orders);
}

int FastBook::tick_index(Price price) const {
  if (!band_set_) return -1;
  const Price off = price - base_;
  if (off < 0 || off % cfg_.tick != 0) return -1;
  const Price idx = off / cfg_.tick;
  if (idx >= static_cast<Price>(ladder_.size())) return -1;
  return static_cast<int>(idx);
}

FastBook::Level& FastBook::level_for(Side side, Price price) {
  const int t = tick_index(price);
  if (t >= 0) return ladder_[static_cast<std::size_t>(t)];
  return side == Side::Buy ? fb_bids_[price] : fb_asks_[price];
}

void FastBook::erase_level_if_fallback(Side side, Price price,
                                       const Level& lvl) {
  if (lvl.head != kNil || tick_index(price) >= 0) return;
  if (side == Side::Buy) {
    fb_bids_.erase(price);
  } else {
    fb_asks_.erase(price);
  }
}

std::uint32_t FastBook::alloc_order(OrderId id, Side side, Price price,
                                    Qty qty) {
  std::uint32_t idx;
  if (!free_.empty()) {
    idx = free_.back();
    free_.pop_back();
  } else {
    idx = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back({});
  }
  pool_[idx] = {id, price, qty, kNil, kNil, side};
  return idx;
}

void FastBook::free_order(std::uint32_t idx) { free_.push_back(idx); }

void FastBook::unlink(Level& lvl, std::uint32_t idx) {
  FOrder& o = pool_[idx];
  if (o.prev != kNil) {
    pool_[o.prev].next = o.next;
  } else {
    lvl.head = o.next;
  }
  if (o.next != kNil) {
    pool_[o.next].prev = o.prev;
  } else {
    lvl.tail = o.prev;
  }
  o.prev = o.next = kNil;
}

void FastBook::rest(OrderId id, Side side, Price price, Qty qty) {
  if (!band_set_) {
    base_ = price - static_cast<Price>(cfg_.band_half_ticks) * cfg_.tick;
    ladder_.assign(2 * static_cast<std::size_t>(cfg_.band_half_ticks) + 1,
                   Level{});
    bid_cursor_ = -1;
    ask_cursor_ = static_cast<int>(ladder_.size());
    band_set_ = true;
  }
  Level& lvl = level_for(side, price);
  const std::uint32_t idx = alloc_order(id, side, price, qty);
  FOrder& o = pool_[idx];
  o.prev = lvl.tail;
  if (lvl.tail != kNil) {
    pool_[lvl.tail].next = idx;
  } else {
    lvl.head = idx;
  }
  lvl.tail = idx;
  lvl.total += qty;
  index_.insert(id, idx);

  const int t = tick_index(price);
  if (t >= 0) {
    if (side == Side::Buy && t > bid_cursor_) bid_cursor_ = t;
    if (side == Side::Sell && t < ask_cursor_) ask_cursor_ = t;
  }
}

void FastBook::remove_resting(std::uint32_t idx) {
  const FOrder o = pool_[idx];  // copy before unlink/free
  Level& lvl = level_for(o.side, o.price);
  lvl.total -= o.open;
  unlink(lvl, idx);
  index_.erase(o.id);
  free_order(idx);
  erase_level_if_fallback(o.side, o.price, lvl);
}

int FastBook::ladder_best_bid() const {
  int t = std::min(bid_cursor_, static_cast<int>(ladder_.size()) - 1);
  while (t >= 0) {
    const Level& lvl = ladder_[static_cast<std::size_t>(t)];
    if (lvl.head != kNil && pool_[lvl.head].side == Side::Buy) break;
    --t;
  }
  bid_cursor_ = t;
  return t;
}

int FastBook::ladder_best_ask() const {
  const int n = static_cast<int>(ladder_.size());
  int t = std::max(ask_cursor_, 0);
  while (t < n) {
    const Level& lvl = ladder_[static_cast<std::size_t>(t)];
    if (lvl.head != kNil && pool_[lvl.head].side == Side::Sell) break;
    ++t;
  }
  ask_cursor_ = t;
  return t < n ? t : -1;
}

std::optional<Price> FastBook::best_bid() const {
  std::optional<Price> best;
  if (band_set_) {
    const int t = ladder_best_bid();
    if (t >= 0) best = price_at(t);
  }
  if (!fb_bids_.empty()) {
    const Price fb = fb_bids_.begin()->first;
    if (!best || fb > *best) best = fb;
  }
  return best;
}

std::optional<Price> FastBook::best_ask() const {
  std::optional<Price> best;
  if (band_set_) {
    const int t = ladder_best_ask();
    if (t >= 0) best = price_at(t);
  }
  if (!fb_asks_.empty()) {
    const Price fb = fb_asks_.begin()->first;
    if (!best || fb < *best) best = fb;
  }
  return best;
}

bool FastBook::would_cross(Side side, Price price) const {
  const auto opposite = side == Side::Buy ? best_ask() : best_bid();
  return opposite && crosses(side, price, *opposite);
}

Qty FastBook::consume_level(Level& lvl, Price level_price, OrderId aggressor,
                            Side aggressor_side, Qty want) {
  Qty filled = 0;
  while (want > 0 && lvl.head != kNil) {
    const std::uint32_t idx = lvl.head;
    FOrder& o = pool_[idx];
    const Qty exec = want < o.open ? want : o.open;
    o.open -= exec;
    lvl.total -= exec;
    want -= exec;
    filled += exec;
    listener_.on_executed({o.id, aggressor, aggressor_side, level_price, exec,
                           next_match_id_++});
    if (o.open == 0) {
      index_.erase(o.id);
      unlink(lvl, idx);
      free_order(idx);
    }
  }
  return filled;
}

Qty FastBook::match(OrderId id, Side side, Price limit, Qty qty) {
  Qty filled = 0;
  while (qty > 0) {
    // Best opposite level comes from the ladder or the fallback map,
    // whichever has the better price (a price lives in exactly one).
    Price best_price = 0;
    Level* lvl = nullptr;
    bool from_fallback = false;

    if (side == Side::Buy) {
      const int t = band_set_ ? ladder_best_ask() : -1;
      if (t >= 0) {
        best_price = price_at(t);
        lvl = &ladder_[static_cast<std::size_t>(t)];
      }
      if (!fb_asks_.empty() &&
          (lvl == nullptr || fb_asks_.begin()->first < best_price)) {
        best_price = fb_asks_.begin()->first;
        lvl = &fb_asks_.begin()->second;
        from_fallback = true;
      }
    } else {
      const int t = band_set_ ? ladder_best_bid() : -1;
      if (t >= 0) {
        best_price = price_at(t);
        lvl = &ladder_[static_cast<std::size_t>(t)];
      }
      if (!fb_bids_.empty() &&
          (lvl == nullptr || fb_bids_.begin()->first > best_price)) {
        best_price = fb_bids_.begin()->first;
        lvl = &fb_bids_.begin()->second;
        from_fallback = true;
      }
    }

    if (lvl == nullptr || !crosses(side, limit, best_price)) break;

    const Qty got = consume_level(*lvl, best_price, id, side, qty);
    qty -= got;
    filled += got;
    if (lvl->head == kNil && from_fallback) {
      if (side == Side::Buy) {
        fb_asks_.erase(best_price);
      } else {
        fb_bids_.erase(best_price);
      }
    }
  }
  return filled;
}

AddResult FastBook::add(OrderId id, Side side, Price price, Qty qty) {
  if (price <= 0) return {AddStatus::BadPrice, 0, 0};
  if (qty == 0) return {AddStatus::BadQty, 0, 0};
  if (index_.contains(id)) return {AddStatus::DuplicateId, 0, 0};

  const Qty filled = match(id, side, price, qty);
  const Qty resting = qty - filled;
  if (resting > 0) {
    rest(id, side, price, resting);
    listener_.on_added({id, side, price, resting});
  }
  return {AddStatus::Ok, filled, resting};
}

bool FastBook::cancel(OrderId id, Qty keep_qty) {
  const auto* pidx = index_.find(id);
  if (pidx == nullptr) return false;
  const std::uint32_t idx = *pidx;
  FOrder& o = pool_[idx];
  if (keep_qty >= o.open) return false;

  if (keep_qty == 0) {
    const Qty canceled = o.open;
    remove_resting(idx);
    listener_.on_canceled({id, canceled, true});
  } else {
    const Qty canceled = o.open - keep_qty;
    o.open = keep_qty;
    level_for(o.side, o.price).total -= canceled;
    listener_.on_canceled({id, canceled, false});
  }
  return true;
}

bool FastBook::replace(OrderId old_id, OrderId new_id, Price new_price,
                       Qty new_qty) {
  const auto* pidx = index_.find(old_id);
  if (pidx == nullptr) return false;
  if (new_id == old_id || index_.contains(new_id)) return false;
  if (new_price <= 0 || new_qty == 0) return false;

  const std::uint32_t idx = *pidx;
  const Side side = pool_[idx].side;
  const Qty old_open = pool_[idx].open;
  remove_resting(idx);

  if (would_cross(side, new_price)) {
    listener_.on_canceled({old_id, old_open, true});
    const Qty filled = match(new_id, side, new_price, new_qty);
    const Qty resting = new_qty - filled;
    if (resting > 0) {
      rest(new_id, side, new_price, resting);
      listener_.on_added({new_id, side, new_price, resting});
    }
  } else {
    rest(new_id, side, new_price, new_qty);
    listener_.on_replaced({old_id, new_id, side, new_price, new_qty});
  }
  return true;
}

BookSnapshot FastBook::snapshot() const {
  std::vector<std::pair<Price, const Level*>> bids, asks;
  for (std::size_t i = 0; i < ladder_.size(); ++i) {
    const Level& lvl = ladder_[i];
    if (lvl.head == kNil) continue;
    const Price px = price_at(static_cast<int>(i));
    (pool_[lvl.head].side == Side::Buy ? bids : asks).emplace_back(px, &lvl);
  }
  for (const auto& [px, lvl] : fb_bids_) bids.emplace_back(px, &lvl);
  for (const auto& [px, lvl] : fb_asks_) asks.emplace_back(px, &lvl);
  std::sort(bids.begin(), bids.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::sort(asks.begin(), asks.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  BookSnapshot snap;
  const auto fill = [&](const auto& src, std::vector<SnapshotLevel>& out) {
    for (const auto& [px, lvl] : src) {
      SnapshotLevel sl{px, {}};
      for (std::uint32_t i = lvl->head; i != kNil; i = pool_[i].next)
        sl.orders.push_back({pool_[i].id, pool_[i].open});
      out.push_back(std::move(sl));
    }
  };
  fill(bids, snap.bids);
  fill(asks, snap.asks);
  return snap;
}

void FastBook::check_invariants() const {
  const auto fail = [](const std::string& what) {
    throw std::logic_error("FastBook: " + what);
  };

  std::size_t count = 0;
  const auto check_level = [&](Price price, const Level& lvl,
                               bool fallback_side_known, Side expect_side) {
    if (lvl.head == kNil) {
      if (lvl.total != 0) fail("empty level with nonzero total");
      if (lvl.tail != kNil) fail("empty level with tail set");
      return;
    }
    Qty sum = 0;
    std::uint32_t prev = kNil;
    const Side side = pool_[lvl.head].side;
    if (fallback_side_known && side != expect_side)
      fail("fallback level holds wrong side");
    for (std::uint32_t i = lvl.head; i != kNil; i = pool_[i].next) {
      const FOrder& o = pool_[i];
      ++count;
      if (o.open == 0) fail("resting order with zero open");
      if (o.price != price) fail("order price != level price");
      if (o.side != side) fail("mixed sides within level");
      if (o.prev != prev) fail("broken prev link");
      const auto* pidx = index_.find(o.id);
      if (pidx == nullptr || *pidx != i) fail("index does not map to order");
      sum += o.open;
      prev = i;
    }
    if (prev != lvl.tail) fail("tail link mismatch");
    if (sum != lvl.total) fail("level total != sum of open");
  };

  for (std::size_t i = 0; i < ladder_.size(); ++i)
    check_level(price_at(static_cast<int>(i)), ladder_[i], false, Side::Buy);
  for (const auto& [px, lvl] : fb_bids_) {
    if (lvl.head == kNil) fail("empty fallback bid level not erased");
    check_level(px, lvl, true, Side::Buy);
  }
  for (const auto& [px, lvl] : fb_asks_) {
    if (lvl.head == kNil) fail("empty fallback ask level not erased");
    check_level(px, lvl, true, Side::Sell);
  }

  if (count != index_.size()) fail("index size != resting order count");

  const auto bb = best_bid();
  const auto ba = best_ask();
  if (bb && ba && *bb >= *ba) fail("book crossed or locked at rest");
}

}  // namespace nsq

#include "book/order_book.hpp"

#include <stdexcept>
#include <string>

namespace nsq {

namespace {
bool crosses(Side aggressor_side, Price aggressor_limit, Price resting_price) {
  return aggressor_side == Side::Buy ? resting_price <= aggressor_limit
                                     : resting_price >= aggressor_limit;
}
}  // namespace

template <typename OppositeMap>
Qty OrderBook::match(OppositeMap& opposite, OrderId id, Side side, Price limit,
                     Qty qty) {
  Qty filled = 0;
  while (qty > 0 && !opposite.empty()) {
    auto level_it = opposite.begin();
    if (!crosses(side, limit, level_it->first)) break;
    Level& level = level_it->second;
    while (qty > 0 && !level.orders.empty()) {
      Order& resting = level.orders.front();
      const Qty exec = qty < resting.open ? qty : resting.open;
      resting.open -= exec;
      level.total -= exec;
      qty -= exec;
      filled += exec;
      listener_.on_executed({resting.id, id, side, resting.price, exec,
                             next_match_id_++});
      if (resting.open == 0) {
        index_.erase(resting.id);
        level.orders.pop_front();
      }
    }
    if (level.orders.empty()) opposite.erase(level_it);
  }
  return filled;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  Level& level = side == Side::Buy ? bids_[price] : asks_[price];
  level.orders.push_back({id, side, price, qty});
  level.total += qty;
  index_.emplace(id, Handle{side, price, std::prev(level.orders.end())});
}

AddResult OrderBook::add(OrderId id, Side side, Price price, Qty qty) {
  if (price <= 0) return {AddStatus::BadPrice, 0, 0};
  if (qty == 0) return {AddStatus::BadQty, 0, 0};
  if (index_.contains(id)) return {AddStatus::DuplicateId, 0, 0};

  const Qty filled = side == Side::Buy ? match(asks_, id, side, price, qty)
                                       : match(bids_, id, side, price, qty);
  const Qty resting = qty - filled;
  if (resting > 0) {
    rest(id, side, price, resting);
    listener_.on_added({id, side, price, resting});
  }
  return {AddStatus::Ok, filled, resting};
}

void OrderBook::remove(std::unordered_map<OrderId, Handle>::iterator idx_it) {
  const Handle& h = idx_it->second;
  auto erase_from = [&](auto& side_map) {
    auto level_it = side_map.find(h.price);
    Level& level = level_it->second;
    level.total -= h.it->open;
    level.orders.erase(h.it);
    if (level.orders.empty()) side_map.erase(level_it);
  };
  if (h.side == Side::Buy) {
    erase_from(bids_);
  } else {
    erase_from(asks_);
  }
  index_.erase(idx_it);
}

bool OrderBook::cancel(OrderId id, Qty keep_qty) {
  auto idx_it = index_.find(id);
  if (idx_it == index_.end()) return false;
  Order& order = *idx_it->second.it;
  if (keep_qty >= order.open) return false;  // nothing to cancel

  if (keep_qty == 0) {
    const Qty canceled = order.open;
    remove(idx_it);
    listener_.on_canceled({id, canceled, true});
  } else {
    const Qty canceled = order.open - keep_qty;
    order.open = keep_qty;
    Level& level = idx_it->second.side == Side::Buy
                       ? bids_.find(idx_it->second.price)->second
                       : asks_.find(idx_it->second.price)->second;
    level.total -= canceled;
    listener_.on_canceled({id, canceled, false});
  }
  return true;
}

bool OrderBook::would_cross(Side side, Price price) const {
  if (side == Side::Buy) {
    return !asks_.empty() && asks_.begin()->first <= price;
  }
  return !bids_.empty() && bids_.begin()->first >= price;
}

bool OrderBook::replace(OrderId old_id, OrderId new_id, Price new_price,
                        Qty new_qty) {
  auto idx_it = index_.find(old_id);
  if (idx_it == index_.end()) return false;
  if (new_id == old_id || index_.contains(new_id)) return false;
  if (new_price <= 0 || new_qty == 0) return false;

  const Side side = idx_it->second.side;
  const Qty old_open = idx_it->second.it->open;
  remove(idx_it);

  if (would_cross(side, new_price)) {
    // A replace that would cross cannot appear as an ITCH replace (replaced
    // orders rest); emit it as a delete plus a fresh order lifecycle.
    listener_.on_canceled({old_id, old_open, true});
    const Qty filled = side == Side::Buy
                           ? match(asks_, new_id, side, new_price, new_qty)
                           : match(bids_, new_id, side, new_price, new_qty);
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

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

BookSnapshot OrderBook::snapshot() const {
  BookSnapshot snap;
  auto fill = [](const auto& side_map, std::vector<SnapshotLevel>& out) {
    for (const auto& [price, level] : side_map) {
      SnapshotLevel sl{price, {}};
      for (const Order& o : level.orders) sl.orders.push_back({o.id, o.open});
      out.push_back(std::move(sl));
    }
  };
  fill(bids_, snap.bids);
  fill(asks_, snap.asks);
  return snap;
}

void OrderBook::check_invariants() const {
  auto fail = [](const std::string& what) { throw std::logic_error(what); };

  std::size_t order_count = 0;
  auto check_side = [&](const auto& side_map, Side side) {
    for (const auto& [price, level] : side_map) {
      if (level.orders.empty()) fail("empty price level not erased");
      Qty sum = 0;
      for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
        const Order& o = *it;
        ++order_count;
        if (o.open == 0) fail("resting order with zero open qty");
        if (o.price != price) fail("order price != level price");
        if (o.side != side) fail("order side != book side");
        sum += o.open;
        auto idx = index_.find(o.id);
        if (idx == index_.end()) fail("resting order missing from index");
        if (idx->second.it != it) fail("index handle does not point at order");
        if (idx->second.price != price || idx->second.side != side)
          fail("index handle side/price mismatch");
      }
      if (sum != level.total) fail("level total != sum of open quantities");
    }
  };
  check_side(bids_, Side::Buy);
  check_side(asks_, Side::Sell);
  if (order_count != index_.size()) fail("index size != resting order count");
  if (!bids_.empty() && !asks_.empty() &&
      bids_.begin()->first >= asks_.begin()->first)
    fail("book is crossed or locked at rest");
}

// Explicit instantiations keep match() out of the header.
template Qty OrderBook::match<OrderBook::BidMap>(BidMap&, OrderId, Side, Price,
                                                 Qty);
template Qty OrderBook::match<OrderBook::AskMap>(AskMap&, OrderId, Side, Price,
                                                 Qty);

}  // namespace nsq

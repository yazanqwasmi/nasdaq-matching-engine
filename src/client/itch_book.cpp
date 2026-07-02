#include "client/itch_book.hpp"

#include <algorithm>

namespace nsq::client {

bool ItchBookBuilder::on_mold_packet(const std::uint8_t* data,
                                     std::size_t n) {
  const auto pkt = mold::parse(data, n);
  if (!pkt) return false;
  gaps_.on_packet(pkt->seq, pkt->messages.size());
  for (const auto& bytes : pkt->messages) {
    if (const auto msg = itch::decode(bytes.data(), bytes.size()))
      on_message(*msg);
  }
  return pkt->end_of_session;
}

void ItchBookBuilder::on_message(const itch::Message& msg) {
  ++messages_;
  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, itch::AddOrder>) {
          last_ts_ = m.timestamp;
          add_order(m.order_ref, m.stock, m.side, m.price, m.shares);
        } else if constexpr (std::is_same_v<T, itch::AddOrderMpid>) {
          last_ts_ = m.add.timestamp;
          add_order(m.add.order_ref, m.add.stock, m.add.side, m.add.price,
                    m.add.shares);
        } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
          last_ts_ = m.timestamp;
          if (const auto it = orders_.find(m.order_ref); it != orders_.end())
            last_trade_price_ = it->second.price;
          last_trade_qty_ = m.executed_shares;
          executed_shares_ += m.executed_shares;
          reduce_order(m.order_ref, m.executed_shares);
        } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
          last_ts_ = m.exec.timestamp;
          last_trade_price_ = m.execution_price;
          last_trade_qty_ = m.exec.executed_shares;
          executed_shares_ += m.exec.executed_shares;
          reduce_order(m.exec.order_ref, m.exec.executed_shares);
        } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
          last_ts_ = m.timestamp;
          reduce_order(m.order_ref, m.canceled_shares);
        } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
          last_ts_ = m.timestamp;
          remove_order(m.order_ref);
        } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
          last_ts_ = m.timestamp;
          const auto it = orders_.find(m.original_ref);
          if (it == orders_.end()) return;
          const Symbol symbol = it->second.symbol;
          const Side side = it->second.side;
          remove_order(m.original_ref);
          add_order(m.new_ref, symbol, side, m.price, m.shares);
        }
        // SystemEvent / StockDirectory / TradingAction / Trade don't change
        // displayed book state in this subset.
      },
      msg);
}

void ItchBookBuilder::add_order(std::uint64_t ref, const Symbol& symbol,
                                Side side, Price price, Qty shares) {
  orders_[ref] = {symbol, side, price, shares};
  auto& book = books_[symbol];
  if (side == Side::Buy) {
    book.bids[price].push_back(ref);
  } else {
    book.asks[price].push_back(ref);
  }
}

void ItchBookBuilder::reduce_order(std::uint64_t ref, Qty by) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) return;
  if (by >= it->second.qty) {
    remove_order(ref);
  } else {
    it->second.qty -= by;
  }
}

void ItchBookBuilder::remove_order(std::uint64_t ref) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) return;
  auto& book = books_[it->second.symbol];
  auto erase_from = [&](auto& side_map) {
    const auto lvl = side_map.find(it->second.price);
    if (lvl == side_map.end()) return;
    lvl->second.remove(ref);
    if (lvl->second.empty()) side_map.erase(lvl);
  };
  if (it->second.side == Side::Buy) {
    erase_from(book.bids);
  } else {
    erase_from(book.asks);
  }
  orders_.erase(it);
}

BookSnapshot ItchBookBuilder::snapshot(const Symbol& symbol) const {
  BookSnapshot snap;
  const auto it = books_.find(symbol);
  if (it == books_.end()) return snap;
  const auto fill = [&](const auto& side_map,
                        std::vector<SnapshotLevel>& out) {
    for (const auto& [price, refs] : side_map) {
      SnapshotLevel lvl{price, {}};
      for (const auto ref : refs)
        lvl.orders.push_back({ref, orders_.at(ref).qty});
      out.push_back(std::move(lvl));
    }
  };
  fill(it->second.bids, snap.bids);
  fill(it->second.asks, snap.asks);
  return snap;
}

std::vector<Symbol> ItchBookBuilder::symbols() const {
  std::vector<Symbol> out;
  for (const auto& [sym, book] : books_) out.push_back(sym);
  return out;
}

}  // namespace nsq::client

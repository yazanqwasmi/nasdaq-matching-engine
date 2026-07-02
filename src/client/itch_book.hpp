// Client-side ITCH book reconstruction: consumes MoldUDP64 packets (or raw
// ITCH messages), maintains per-symbol books, and produces snapshots
// directly comparable to the engine's OrderBook::snapshot(). Used by the
// feed-integrity tests, itchlisten, and the terminal viewer.
#pragma once

#include "book/order_book.hpp"  // BookSnapshot
#include "itch/itch.hpp"
#include "mold/mold.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace nsq::client {

class ItchBookBuilder {
 public:
  // Returns true when the packet is end-of-session.
  bool on_mold_packet(const std::uint8_t* data, std::size_t n);

  void on_message(const itch::Message& msg);

  BookSnapshot snapshot(const Symbol& symbol) const;
  std::vector<Symbol> symbols() const;

  bool had_gap() const { return gaps_.had_gap(); }
  std::uint64_t message_count() const { return messages_; }
  std::uint64_t last_timestamp() const { return last_ts_; }

 private:
  struct BuilderOrder {
    Symbol symbol;
    Side side;
    Price price;
    Qty qty;
  };
  struct SymbolBook {
    std::map<Price, std::list<std::uint64_t>, std::greater<>> bids;
    std::map<Price, std::list<std::uint64_t>> asks;
  };

  void add_order(std::uint64_t ref, const Symbol& symbol, Side side,
                 Price price, Qty shares);
  void reduce_order(std::uint64_t ref, Qty by);
  void remove_order(std::uint64_t ref);

  std::unordered_map<std::uint64_t, BuilderOrder> orders_;
  std::map<Symbol, SymbolBook> books_;
  mold::GapDetector gaps_;
  std::uint64_t messages_ = 0;
  std::uint64_t last_ts_ = 0;
};

}  // namespace nsq::client

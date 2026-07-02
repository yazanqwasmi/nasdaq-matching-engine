// In-process demo: drives the reference book with a scripted scenario and a
// random burst, printing every event and the final ladder. Serves as a living
// example of the book API; no protocols or sockets involved.
#include "book/order_book.hpp"

#include <cstdio>
#include <random>

namespace nsq {
namespace {

struct PrintingListener : BookListener {
  bool verbose = true;
  void on_added(const AddedEvent& e) override {
    if (verbose)
      std::printf("  ADDED    #%llu %c %u @ %s\n",
                  static_cast<unsigned long long>(e.id),
                  static_cast<char>(e.side), e.qty,
                  price_to_string(e.price).c_str());
  }
  void on_executed(const ExecutedEvent& e) override {
    if (verbose)
      std::printf("  EXEC     #%llu x #%llu  %u @ %s (match %llu)\n",
                  static_cast<unsigned long long>(e.resting_id),
                  static_cast<unsigned long long>(e.aggressor_id), e.qty,
                  price_to_string(e.price).c_str(),
                  static_cast<unsigned long long>(e.match_id));
  }
  void on_canceled(const CanceledEvent& e) override {
    if (verbose)
      std::printf("  CANCELED #%llu -%u%s\n",
                  static_cast<unsigned long long>(e.id), e.canceled_qty,
                  e.removed ? " (removed)" : "");
  }
  void on_replaced(const ReplacedEvent& e) override {
    if (verbose)
      std::printf("  REPLACED #%llu -> #%llu %u @ %s\n",
                  static_cast<unsigned long long>(e.old_id),
                  static_cast<unsigned long long>(e.new_id), e.qty,
                  price_to_string(e.price).c_str());
  }
};

void print_ladder(const OrderBook& book, int depth) {
  const auto snap = book.snapshot();
  std::printf("\n  %-22s | %s\n  %s\n", "BID", "ASK",
              "-----------------------+-----------------------");
  for (int i = 0; i < depth; ++i) {
    const auto row = static_cast<std::size_t>(i);
    char bid[64] = "", ask[64] = "";
    if (row < snap.bids.size()) {
      Qty total = 0;
      for (const auto& o : snap.bids[row].orders) total += o.qty;
      std::snprintf(bid, sizeof bid, "%8u  %10s", total,
                    price_to_string(snap.bids[row].price).c_str());
    }
    if (row < snap.asks.size()) {
      Qty total = 0;
      for (const auto& o : snap.asks[row].orders) total += o.qty;
      std::snprintf(ask, sizeof ask, "%-10s  %8u",
                    price_to_string(snap.asks[row].price).c_str(), total);
    }
    if (bid[0] == '\0' && ask[0] == '\0') break;
    std::printf("  %-22s | %s\n", bid, ask);
  }
  std::printf("\n");
}

int run() {
  PrintingListener events;
  OrderBook book{events};

  std::puts("== Scripted scenario ==");
  std::puts("Build a two-sided book:");
  book.add(1, Side::Buy, make_price(99, 5000), 300);
  book.add(2, Side::Buy, make_price(99, 0), 500);
  book.add(3, Side::Sell, make_price(100, 5000), 400);
  book.add(4, Side::Sell, make_price(101, 0), 200);
  print_ladder(book, 5);

  std::puts("Aggressive buy 500 @ 100.50 walks the ask:");
  book.add(5, Side::Buy, make_price(100, 5000), 500);
  print_ladder(book, 5);

  std::puts("Cancel #2, replace #1 to 99.75 x 350 (loses time priority):");
  book.cancel(2);
  book.replace(1, 6, make_price(99, 7500), 350);
  print_ladder(book, 5);

  std::puts("== Random burst: 2000 orders around 100.00 ==");
  events.verbose = false;
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<Price> tick(-20, 20);
  std::uniform_int_distribution<Qty> qty(1, 500);
  for (OrderId id = 100; id < 2100; ++id) {
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    book.add(id, side, make_price(100, 0) + tick(rng) * make_price(0, 100),
             qty(rng));
  }
  book.check_invariants();
  std::puts("Invariants hold. Final book (top 8):");
  print_ladder(book, 8);
  return 0;
}

}  // namespace
}  // namespace nsq

int main() { return nsq::run(); }

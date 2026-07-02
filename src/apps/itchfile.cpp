// Reconstruct order books from a NASDAQ raw-ITCH 5.0 file (e.g. the free
// TotalView-ITCH sample days from emi.nasdaq.com).
//   itchfile <path|-> [--symbol AAPL]... [--depth N] [--max-messages M]
// Use '-' to stream from stdin:  gzcat 12302019.NASDAQ_ITCH50.gz | itchfile -
#include "client/itch_file_processor.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void print_ladder(const nsq::BookSnapshot& snap, int depth) {
  std::printf("  %-22s | %s\n  %s\n", "BID", "ASK",
              "-----------------------+-----------------------");
  for (int i = 0; i < depth; ++i) {
    const auto row = static_cast<std::size_t>(i);
    char bid[64] = "", ask[64] = "";
    if (row < snap.bids.size()) {
      nsq::Qty total = 0;
      for (const auto& o : snap.bids[row].orders) total += o.qty;
      std::snprintf(bid, sizeof bid, "%8u  %10s", total,
                    nsq::price_to_string(snap.bids[row].price).c_str());
    }
    if (row < snap.asks.size()) {
      nsq::Qty total = 0;
      for (const auto& o : snap.asks[row].orders) total += o.qty;
      std::snprintf(ask, sizeof ask, "%-10s  %8u",
                    nsq::price_to_string(snap.asks[row].price).c_str(), total);
    }
    if (bid[0] == '\0' && ask[0] == '\0') break;
    std::printf("  %-22s | %s\n", bid, ask);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: itchfile <path|-> [--symbol SYM]... [--depth N] "
                 "[--max-messages M]\n");
    return 1;
  }
  const std::string path = argv[1];
  std::vector<nsq::Symbol> symbols;
  int depth = 10;
  std::uint64_t max_messages = 0;
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--symbol") == 0)
      symbols.push_back(nsq::make_symbol(argv[i + 1]));
    else if (std::strcmp(argv[i], "--depth") == 0)
      depth = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--max-messages") == 0)
      max_messages = static_cast<std::uint64_t>(std::atoll(argv[i + 1]));
  }

  std::FILE* f = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "itchfile: cannot open %s\n", path.c_str());
    return 1;
  }

  nsq::client::ItchBookBuilder builder;
  nsq::client::ItchFileStats stats;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok =
      nsq::client::process_raw_itch(f, symbols, max_messages, builder, stats);
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  if (f != stdin) std::fclose(f);
  if (!ok) {
    std::fprintf(stderr, "itchfile: framing error: %s\n", stats.error.c_str());
    return 2;
  }

  std::printf("itchfile: %llu messages in %.1fs (%.1fM msgs/sec)\n",
              static_cast<unsigned long long>(stats.total), secs,
              static_cast<double>(stats.total) / secs / 1e6);
  std::printf("%-6s %12s %12s\n", "type", "decoded", "skipped");
  for (int t = 0; t < 256; ++t) {
    const auto ut = static_cast<std::size_t>(t);
    if (stats.decoded[ut] == 0 && stats.skipped[ut] == 0) continue;
    std::printf("%-6c %12llu %12llu\n", t,
                static_cast<unsigned long long>(stats.decoded[ut]),
                static_cast<unsigned long long>(stats.skipped[ut]));
  }
  std::printf(
      "symbols tracked: %zu   filtered adds: %llu   executed shares: %llu\n",
      builder.symbols().size(),
      static_cast<unsigned long long>(stats.filtered),
      static_cast<unsigned long long>(builder.executed_shares()));

  for (const auto& sym : builder.symbols()) {
    const auto sv = nsq::symbol_view(sym);
    std::printf("\n%.*s (top %d):\n", static_cast<int>(sv.size()), sv.data(),
                depth);
    print_ladder(builder.snapshot(sym), depth);
  }
  return 0;
}

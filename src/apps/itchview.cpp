// Live terminal book viewer: joins the ITCH multicast feed and redraws a
// depth ladder ~10x/second using ANSI escape codes (no curses dependency).
// Shows aggregate size per level, order counts, spread, last trade, and
// feed statistics. Ctrl-C to exit.
#include "client/itch_book.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <string>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

void draw(const nsq::client::ItchBookBuilder& builder,
          const nsq::Symbol& symbol, int depth, double msg_rate) {
  using nsq::Qty;
  const auto snap = builder.snapshot(symbol);
  std::string out;
  out += "\x1b[H\x1b[2J";  // home + clear

  const auto sv = nsq::symbol_view(symbol);
  char line[160];
  std::snprintf(line, sizeof line,
                "  %.*s   msgs=%llu (%.0f/s)   traded=%llu   gaps=%s\n",
                static_cast<int>(sv.size()), sv.data(),
                static_cast<unsigned long long>(builder.message_count()),
                msg_rate,
                static_cast<unsigned long long>(builder.executed_shares()),
                builder.had_gap() ? "YES" : "none");
  out += line;

  if (builder.last_trade_price() > 0) {
    std::snprintf(line, sizeof line, "  last trade: %u @ %s\n",
                  builder.last_trade_qty(),
                  nsq::price_to_string(builder.last_trade_price()).c_str());
    out += line;
  }
  if (!snap.bids.empty() && !snap.asks.empty()) {
    std::snprintf(
        line, sizeof line, "  spread: %s\n",
        nsq::price_to_string(snap.asks[0].price - snap.bids[0].price).c_str());
    out += line;
  }

  std::snprintf(line, sizeof line, "\n  %6s %10s %12s | %-12s %-10s %-6s\n",
                "#ord", "SIZE", "BID", "ASK", "SIZE", "#ord");
  out += line;
  out += "  ------------------------------+------------------------------\n";
  for (int i = 0; i < depth; ++i) {
    const auto row = static_cast<std::size_t>(i);
    char bid[64] = "", ask[64] = "";
    if (row < snap.bids.size()) {
      Qty total = 0;
      for (const auto& o : snap.bids[row].orders) total += o.qty;
      std::snprintf(bid, sizeof bid, "%6zu %10u %12s",
                    snap.bids[row].orders.size(), total,
                    nsq::price_to_string(snap.bids[row].price).c_str());
    }
    if (row < snap.asks.size()) {
      Qty total = 0;
      for (const auto& o : snap.asks[row].orders) total += o.qty;
      std::snprintf(ask, sizeof ask, "%-12s %-10u %-6zu",
                    nsq::price_to_string(snap.asks[row].price).c_str(), total,
                    snap.asks[row].orders.size());
    }
    std::snprintf(line, sizeof line, "  %30s | %-30s\n", bid, ask);
    out += line;
  }
  out += "\n  Ctrl-C to exit\n";
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const char* group = arg_value(argc, argv, "--group", "239.192.0.1");
  const auto port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--port", "26000")));
  const std::string symbol_arg = arg_value(argc, argv, "--symbol", "AAPL");
  const int depth = std::atoi(arg_value(argc, argv, "--depth", "10"));
  const nsq::Symbol symbol = nsq::make_symbol(symbol_arg);

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "itchview: bind failed: %s\n", strerror(errno));
    return 1;
  }
  ip_mreq mreq{};
  ::inet_pton(AF_INET, group, &mreq.imr_multiaddr);
  ::inet_pton(AF_INET, "127.0.0.1", &mreq.imr_interface);
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) !=
      0) {
    std::fprintf(stderr, "itchview: join failed: %s\n", strerror(errno));
    return 1;
  }
  timeval tv{0, 100'000};  // 100ms so redraws stay responsive
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  std::signal(SIGINT, on_sigint);
  nsq::client::ItchBookBuilder builder;

  using clock = std::chrono::steady_clock;
  auto last_draw = clock::now();
  auto rate_mark = last_draw;
  std::uint64_t rate_msgs = 0;
  double msg_rate = 0.0;

  while (!g_stop) {
    std::uint8_t buf[2048];
    const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n > 0) {
      if (builder.on_mold_packet(buf, static_cast<std::size_t>(n))) break;
    }
    const auto now = clock::now();
    if (now - last_draw > std::chrono::milliseconds(100)) {
      if (now - rate_mark > std::chrono::seconds(1)) {
        msg_rate = static_cast<double>(builder.message_count() - rate_msgs) /
                   std::chrono::duration<double>(now - rate_mark).count();
        rate_msgs = builder.message_count();
        rate_mark = now;
      }
      draw(builder, symbol, depth, msg_rate);
      last_draw = now;
    }
  }
  ::close(fd);
  std::puts("\nitchview: bye");
  return 0;
}

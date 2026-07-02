// ITCH multicast listener: joins the feed group, reconstructs books from
// raw ITCH-in-MoldUDP64, verifies gap-free sequencing, and prints the final
// ladders. Exits on end-of-session, --seconds elapsing, or Ctrl-C. Returns
// nonzero if a sequence gap was detected.
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

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

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
  const char* group = arg_value(argc, argv, "--group", "239.192.0.1");
  const auto port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--port", "26000")));
  const int seconds = std::atoi(arg_value(argc, argv, "--seconds", "0"));

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "itchlisten: bind failed: %s\n", strerror(errno));
    return 1;
  }

  ip_mreq mreq{};
  ::inet_pton(AF_INET, group, &mreq.imr_multiaddr);
  ::inet_pton(AF_INET, "127.0.0.1", &mreq.imr_interface);
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) !=
      0) {
    std::fprintf(stderr, "itchlisten: join %s failed: %s\n", group,
                 strerror(errno));
    return 1;
  }
  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  std::printf("itchlisten: joined %s:%u\n", group, port);
  std::signal(SIGINT, on_sigint);

  nsq::client::ItchBookBuilder builder;
  std::uint64_t packets = 0;
  time_t start = time(nullptr);
  bool eos = false;
  while (!g_stop && !eos) {
    if (seconds > 0 && time(nullptr) - start >= seconds) break;
    std::uint8_t buf[2048];
    const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
    if (n <= 0) continue;  // timeout: re-check stop conditions
    ++packets;
    eos = builder.on_mold_packet(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);

  std::printf("\nitchlisten: %llu packets, %llu messages, gaps=%s%s\n",
              static_cast<unsigned long long>(packets),
              static_cast<unsigned long long>(builder.message_count()),
              builder.had_gap() ? "YES" : "none",
              eos ? " (end of session)" : "");
  for (const auto& sym : builder.symbols()) {
    std::printf("\n%.*s (top 8):\n",
                static_cast<int>(nsq::symbol_view(sym).size()),
                nsq::symbol_view(sym).data());
    print_ladder(builder.snapshot(sym), 8);
  }
  return builder.had_gap() ? 2 : 0;
}

// Random order-flow generator: a minimal OUCH client that logs in over
// SoupBinTCP and fires random enter/cancel flow at the exchange. Grows into
// the multi-agent marketsim later; for the MVP it exists to put a live,
// two-sided market on the feed.
#include "common/types.hpp"
#include "ouch/ouch.hpp"
#include "soup/soup.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {
const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace nsq;
  const char* host = arg_value(argc, argv, "--host", "127.0.0.1");
  const auto port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--port", "26400")));
  const std::string symbol = arg_value(argc, argv, "--symbol", "AAPL");
  const int total = std::atoi(arg_value(argc, argv, "--orders", "2000"));
  const auto seed = static_cast<std::uint64_t>(
      std::atoll(arg_value(argc, argv, "--seed", "1")));

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host, &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "flowgen: connect failed: %s\n", strerror(errno));
    return 1;
  }
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  std::vector<std::uint8_t> out;
  soup::append_login_request(out, "FLOW", "SIM", "", 1);
  (void)!::send(fd, out.data(), out.size(), 0);

  // Wait for login accept, then go non-blocking for the pump loop.
  soup::FrameParser parser;
  bool logged_in = false;
  while (!logged_in) {
    std::uint8_t buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return 1;
    parser.push(buf, static_cast<std::size_t>(n));
    while (auto p = parser.next())
      if (p->type == soup::kLoginAccepted) logged_in = true;
  }
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  std::printf("flowgen: logged in, sending %d orders on %s\n", total,
              symbol.c_str());

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<Price> tick(-25, 25);
  std::uniform_int_distribution<Qty> qty(1, 500);
  std::uniform_int_distribution<int> pct(0, 99);

  std::vector<std::string> live;
  std::uint64_t accepted = 0, executed_shares = 0, canceled = 0;
  int next_token = 1;

  const auto pump = [&] {
    std::uint8_t buf[8192];
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      parser.push(buf, static_cast<std::size_t>(n));
    }
    while (auto p = parser.next()) {
      if (p->type != soup::kSequenced || p->payload.empty()) continue;
      switch (static_cast<char>(p->payload[0])) {
        case 'A': ++accepted; break;
        case 'E': {
          const auto e =
              ouch::decode_executed(p->payload.data(), p->payload.size());
          if (e) executed_shares += e->executed_shares;
          break;
        }
        case 'C': ++canceled; break;
        default: break;
      }
    }
  };

  for (int i = 0; i < total; ++i) {
    if (!live.empty() && pct(rng) < 20) {
      // 20%: cancel a random earlier order (may already be filled).
      const auto k = rng() % live.size();
      ouch::CancelOrder c{};
      c.token = ouch::make_token(live[k]);
      c.shares = 0;
      std::uint8_t msg[ouch::kCancelOrderSize];
      ouch::encode(c, msg);
      out.clear();
      soup::append_packet(out, soup::kUnsequenced, msg, sizeof msg);
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    } else {
      ouch::EnterOrder m{};
      const std::string token = "F" + std::to_string(next_token++);
      m.token = ouch::make_token(token);
      m.side = (rng() & 1) ? Side::Buy : Side::Sell;
      m.shares = qty(rng);
      m.stock = make_symbol(symbol);
      m.price = make_price(100, 0) + tick(rng) * make_price(0, 100);
      m.tif = 99999;
      m.firm = {'F', 'L', 'O', 'W'};
      m.display = 'Y';
      m.capacity = 'A';
      m.intermarket_sweep = 'N';
      m.cross_type = 'N';
      m.customer_type = 'R';
      std::uint8_t msg[ouch::kEnterOrderSize];
      ouch::encode(m, msg);
      out.clear();
      soup::append_packet(out, soup::kUnsequenced, msg, sizeof msg);
      live.push_back(token);
    }
    (void)!::send(fd, out.data(), out.size(), 0);
    pump();
    struct timespec ts {0, 200'000};  // 200us pacing
    nanosleep(&ts, nullptr);
  }

  // Give the exchange a beat to finish responding, then drain.
  for (int i = 0; i < 20; ++i) {
    struct timespec ts {0, 50'000'000};
    nanosleep(&ts, nullptr);
    pump();
  }

  out.clear();
  soup::append_packet(out, soup::kLogoutRequest, nullptr, 0);
  (void)!::send(fd, out.data(), out.size(), 0);
  ::close(fd);

  std::printf(
      "flowgen: done. accepted=%llu executed_shares=%llu canceled=%llu\n",
      static_cast<unsigned long long>(accepted),
      static_cast<unsigned long long>(executed_shares),
      static_cast<unsigned long long>(canceled));
  return 0;
}

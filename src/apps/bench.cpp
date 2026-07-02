// Benchmark harness. Run from a Release build:
//   book    — FastBook vs reference throughput + FastBook per-op latency
//   queue   — MpscQueue vs SpscRing cross-thread hop latency
//   e2e     — full stack over real sockets: OUCH enter -> Accepted RTT and
//             enter -> ITCH datagram out
// Default runs all three.
#include "book/fast_book.hpp"
#include "book/order_book.hpp"
#include "client/ouch_client.hpp"
#include "common/histogram.hpp"
#include "common/queue.hpp"
#include "common/spsc_ring.hpp"
#include "engine/engine.hpp"
#include "feed/feed.hpp"
#include "gateway/gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace nsq {
namespace {

using clock_t_ = std::chrono::steady_clock;

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock_t_::now().time_since_epoch())
          .count());
}

struct Op {
  int kind;
  OrderId id, id2;
  Side side;
  Price px;
  Qty q;
};

std::vector<Op> make_ops(std::size_t n) {
  std::mt19937_64 rng(99);
  std::uniform_int_distribution<int> op(0, 99);
  std::uniform_int_distribution<Price> tick(-60, 60);
  std::uniform_int_distribution<Qty> qty(1, 800);
  const Price mid = make_price(100, 0), cent = make_price(0, 100);
  std::vector<Op> ops;
  ops.reserve(n);
  std::vector<OrderId> live;
  OrderId next = 1;
  for (std::size_t i = 0; i < n; ++i) {
    const int roll = op(rng);
    if (roll < 55 || live.empty()) {
      ops.push_back({0, next, 0, (rng() & 1) ? Side::Buy : Side::Sell,
                     mid + tick(rng) * cent, qty(rng)});
      live.push_back(next++);
      if (live.size() > 20'000)
        live.erase(live.begin(), live.begin() + 10'000);
    } else if (roll < 80) {
      const std::size_t k = rng() % live.size();
      ops.push_back({1, live[k], 0, Side::Buy, 0, 0});
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    } else {
      const std::size_t k = rng() % live.size();
      ops.push_back({2, live[k], next++, Side::Buy, mid + tick(rng) * cent,
                     qty(rng)});
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
    }
  }
  return ops;
}

template <typename Book>
double run_ops(Book& book, const std::vector<Op>& ops) {
  const auto t0 = clock_t_::now();
  for (const auto& o : ops) {
    if (o.kind == 0) {
      book.add(o.id, o.side, o.px, o.q);
    } else if (o.kind == 1) {
      book.cancel(o.id);
    } else {
      book.replace(o.id, o.id2, o.px, o.q);
    }
  }
  return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

void bench_book() {
  std::puts("== book: 2M mixed ops (55% add / 25% cancel / 20% replace) ==");
  const auto ops = make_ops(2'000'000);
  BookListener sink;
  {
    OrderBook ref{sink};
    const double s = run_ops(ref, ops);
    std::printf("  reference  %.2fM ops/sec\n", 2.0 / s);
  }
  {
    FastBook fast{sink};
    const double s = run_ops(fast, ops);
    std::printf("  fastbook   %.2fM ops/sec\n", 2.0 / s);
  }
  // Per-op latency on the FastBook.
  {
    FastBook fast{sink};
    LatencyHistogram h;
    for (const auto& o : ops) {
      const auto t0 = clock_t_::now();
      if (o.kind == 0) {
        fast.add(o.id, o.side, o.px, o.q);
      } else if (o.kind == 1) {
        fast.cancel(o.id);
      } else {
        fast.replace(o.id, o.id2, o.px, o.q);
      }
      h.record(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              clock_t_::now() - t0)
              .count()));
    }
    std::printf("  %s\n", h.summary("fastbook per-op").c_str());
  }
}

void bench_queue() {
  // Ping-pong: bounce one token between two threads through a pair of
  // queues and record half the round-trip as the cross-thread hop cost.
  std::puts("\n== queue: cross-thread ping-pong hop latency (200k round trips) ==");
  constexpr std::uint64_t kRounds = 200'000;
  {
    MpscQueue<std::uint64_t> ping, pong;
    LatencyHistogram h;
    std::thread echo([&] {
      std::uint64_t got = 0;
      while (got < kRounds) {
        for (const auto v : ping.wait_drain()) {
          pong.push(v);
          ++got;
        }
      }
    });
    for (std::uint64_t i = 0; i < kRounds; ++i) {
      const std::uint64_t t0 = now_ns();
      ping.push(i);
      for (bool done = false; !done;)
        for (const auto v : pong.drain()) done = (v == i);
      h.record((now_ns() - t0) / 2);
    }
    ping.stop();
    echo.join();
    std::printf("  %s\n", h.summary("mutex MpscQueue").c_str());
  }
  {
    SpscRing<std::uint64_t, 4096> ping, pong;
    LatencyHistogram h;
    std::thread echo([&] {
      std::uint64_t got = 0;
      while (got < kRounds) {
        if (const auto v = ping.try_pop()) {
          while (!pong.try_push(*v)) {
          }
          ++got;
        }
      }
    });
    for (std::uint64_t i = 0; i < kRounds; ++i) {
      const std::uint64_t t0 = now_ns();
      while (!ping.try_push(i)) {
      }
      for (bool done = false; !done;)
        if (const auto v = pong.try_pop()) done = (*v == i);
      h.record((now_ns() - t0) / 2);
    }
    echo.join();
    std::printf("  %s\n", h.summary("lock-free SpscRing").c_str());
  }
}

struct RttHandler : client::OuchClient::Handler {
  std::uint64_t accepted = 0;
  void on_accepted(const ouch::Accepted&) override { ++accepted; }
};

void bench_e2e() {
  std::puts(
      "\n== end-to-end: OUCH enter -> Accepted RTT and -> ITCH out "
      "(10k orders over real sockets) ==");
  // UDP receiver for the feed.
  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  socklen_t alen = sizeof addr;
  ::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &alen);
  timeval tv{1, 0};
  ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  MpscQueue<engine::Command> to_engine;
  MpscQueue<engine::ClientResponse> to_gateway;
  MpscQueue<engine::MarketEvent> to_feed;
  engine::Engine eng{to_engine, to_gateway, to_feed};
  gateway::Gateway gw{0, to_engine, to_gateway};
  feed::FeedConfig fcfg;
  fcfg.dest_ip = "127.0.0.1";
  fcfg.dest_port = ntohs(addr.sin_port);
  feed::FeedPublisher pub{to_feed, fcfg};
  eng.start();
  gw.start();
  pub.start();

  RttHandler handler;
  client::OuchClient c{handler};
  if (!c.connect("127.0.0.1", gw.port(), "BENCH", "PW")) {
    std::puts("  connect failed");
    return;
  }

  LatencyHistogram rtt, itch_out;
  std::uint8_t buf[2048];
  for (int i = 0; i < 10'000; ++i) {
    // Alternate sides at the same price so every second order trades.
    const Side side = (i & 1) ? Side::Buy : Side::Sell;
    const std::uint64_t t0 = now_ns();
    c.enter("B" + std::to_string(i), side, "AAPL", make_price(100, 0), 100);
    const std::uint64_t want = handler.accepted + 1;
    while (handler.accepted < want) c.poll();
    rtt.record(now_ns() - t0);
    // The resting add (or execution) also hits the feed.
    const ssize_t n = ::recvfrom(rx, buf, sizeof buf, 0, nullptr, nullptr);
    if (n > 0) itch_out.record(now_ns() - t0);
  }
  std::printf("  %s\n", rtt.summary("enter -> Accepted RTT").c_str());
  std::printf("  %s\n", itch_out.summary("enter -> ITCH datagram").c_str());

  c.close();
  gw.stop();
  eng.stop();
  pub.stop();
  ::close(rx);
}

}  // namespace
}  // namespace nsq

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  if (mode == "book" || mode == "all") nsq::bench_book();
  if (mode == "queue" || mode == "all") nsq::bench_queue();
  if (mode == "e2e" || mode == "all") nsq::bench_e2e();
  return 0;
}

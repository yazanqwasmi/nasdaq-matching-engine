// Multi-agent market simulator. Even-numbered agents are market makers
// (quote both sides around a mean-reverting fair value, cancel/replace
// churn); odd-numbered agents are takers (cross the spread at random
// intervals). Each agent runs its own OUCH/SoupBinTCP connection on its own
// thread, so the gateway sees realistic concurrent multi-client flow.
#include "client/ouch_client.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

struct Stats {
  std::atomic<std::uint64_t> accepted{0}, executed_shares{0}, canceled{0},
      rejected{0};
};

struct CountingHandler : nsq::client::OuchClient::Handler {
  Stats& stats;
  explicit CountingHandler(Stats& s) : stats(s) {}
  void on_accepted(const nsq::ouch::Accepted&) override { ++stats.accepted; }
  void on_executed(const nsq::ouch::Executed& m) override {
    stats.executed_shares += m.executed_shares;
  }
  void on_canceled(const nsq::ouch::Canceled&) override { ++stats.canceled; }
  void on_rejected(const nsq::ouch::Rejected&) override { ++stats.rejected; }
};

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void run_agent(int id, const char* host, std::uint16_t port,
               const std::string& symbol, int seconds, std::uint64_t seed,
               Stats& stats) {
  using namespace nsq;
  CountingHandler handler(stats);
  client::OuchClient c{handler};
  if (!c.connect(host, port, "AGENT" + std::to_string(id), "SIM")) {
    std::fprintf(stderr, "agent %d: connect failed\n", id);
    return;
  }

  std::mt19937_64 rng(seed + static_cast<std::uint64_t>(id) * 7919);
  std::uniform_int_distribution<Qty> mk_size(100, 500);
  std::uniform_int_distribution<Qty> tk_size(50, 300);
  std::uniform_int_distribution<int> spread_c(1, 5);
  std::uniform_int_distribution<int> walk(-3, 3);

  const Price cent = make_price(0, 100);
  Price fv = make_price(100, 0);
  const bool maker = (id % 2) == 0;
  int next_token = 1;
  std::string live_bid, live_ask;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    // Mean-reverting random walk keeps the market near $100.
    fv += walk(rng) * cent;
    fv -= (fv - make_price(100, 0)) / 200;

    if (maker) {
      if (!live_bid.empty()) c.cancel(live_bid);
      if (!live_ask.empty()) c.cancel(live_ask);
      live_bid = "B" + std::to_string(next_token++);
      live_ask = "S" + std::to_string(next_token++);
      const Price half = spread_c(rng) * cent;
      c.enter(live_bid, Side::Buy, symbol, fv - half, mk_size(rng));
      c.enter(live_ask, Side::Sell, symbol, fv + half, mk_size(rng));
      c.poll();
      sleep_ms(2 + static_cast<int>(rng() % 8));
    } else {
      const bool buy = rng() & 1;
      const std::string tok = "T" + std::to_string(next_token++);
      // Cross the touch by a few cents so it takes liquidity.
      const Price px = buy ? fv + 6 * cent : fv - 6 * cent;
      c.enter(tok, buy ? Side::Buy : Side::Sell, symbol, px, tk_size(rng));
      c.poll();
      sleep_ms(10 + static_cast<int>(rng() % 40));
    }
  }
  // Drain remaining responses briefly, then log out.
  for (int i = 0; i < 10; ++i) {
    c.poll();
    sleep_ms(20);
  }
  c.close();
}

}  // namespace

int main(int argc, char** argv) {
  const char* host = arg_value(argc, argv, "--host", "127.0.0.1");
  const auto port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--port", "26400")));
  const std::string symbol = arg_value(argc, argv, "--symbol", "AAPL");
  const int agents = std::atoi(arg_value(argc, argv, "--agents", "6"));
  const int seconds = std::atoi(arg_value(argc, argv, "--seconds", "10"));
  const auto seed = static_cast<std::uint64_t>(
      std::atoll(arg_value(argc, argv, "--seed", "1")));

  std::printf("marketsim: %d agents on %s for %ds (%s:%u)\n", agents,
              symbol.c_str(), seconds, host, port);

  Stats stats;
  std::vector<std::thread> threads;
  for (int i = 0; i < agents; ++i)
    threads.emplace_back(run_agent, i, host, port, symbol, seconds, seed,
                         std::ref(stats));
  for (auto& t : threads) t.join();

  std::printf(
      "marketsim: done. accepted=%llu executed_shares=%llu canceled=%llu "
      "rejected=%llu\n",
      static_cast<unsigned long long>(stats.accepted.load()),
      static_cast<unsigned long long>(stats.executed_shares.load()),
      static_cast<unsigned long long>(stats.canceled.load()),
      static_cast<unsigned long long>(stats.rejected.load()));
  return 0;
}

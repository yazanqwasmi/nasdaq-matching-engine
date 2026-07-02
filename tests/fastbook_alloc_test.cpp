// Verifies the FastBook hot path is allocation-free at steady state by
// counting global operator new calls. Lives in its own binary because it
// replaces the global allocator.
#include "book/fast_book.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>

namespace {
std::atomic<std::uint64_t> g_allocs{0};
}

void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace nsq {
namespace {

TEST(FastBookAlloc, SteadyStateHotPathIsAllocationFree) {
  BookListener sink;  // default no-op listener
  FastBook::Config cfg;
  cfg.expected_orders = 1 << 16;
  FastBook book{sink, cfg};

  std::mt19937_64 rng(1);
  std::uniform_int_distribution<Price> tick(-50, 50);
  std::uniform_int_distribution<Qty> qty(1, 500);
  const Price mid = make_price(100, 0);
  const Price cent = make_price(0, 100);

  // Warm up: populate pools, maps, and ladder.
  OrderId id = 1;
  for (int i = 0; i < 20'000; ++i) {
    book.add(id++, (rng() & 1) ? Side::Buy : Side::Sell,
             mid + tick(rng) * cent, qty(rng));
    if ((i & 3) == 0) book.cancel(rng() % id + 1);
  }

  // Steady state: in-band adds/cancels/executions must not allocate.
  const std::uint64_t before = g_allocs.load();
  for (int i = 0; i < 50'000; ++i) {
    book.add(id++, (rng() & 1) ? Side::Buy : Side::Sell,
             mid + tick(rng) * cent, qty(rng));
    if ((i & 3) == 0) book.cancel(rng() % id + 1);
  }
  const std::uint64_t after = g_allocs.load();
  EXPECT_EQ(after - before, 0u)
      << (after - before) << " allocations on the hot path";
}

}  // namespace
}  // namespace nsq

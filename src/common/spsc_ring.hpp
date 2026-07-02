// Lock-free single-producer/single-consumer ring buffer. Capacity is N-1
// (one slot distinguishes full from empty). Head and tail live on separate
// cache lines to avoid false sharing; producer publishes with release,
// consumer observes with acquire. Verified with ThreadSanitizer (see
// latency_test.cpp's two-thread stress test).
#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace nsq {

template <typename T, std::size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  bool try_push(T v) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) & (N - 1);
    if (next == head_.load(std::memory_order_acquire)) return false;  // full
    buf_[tail] = std::move(v);
    tail_.store(next, std::memory_order_release);
    return true;
  }

  std::optional<T> try_pop() {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return std::nullopt;
    T v = std::move(buf_[head]);
    head_.store((head + 1) & (N - 1), std::memory_order_release);
    return v;
  }

 private:
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  alignas(64) T buf_[N];
};

}  // namespace nsq

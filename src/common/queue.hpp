// Mutex-guarded multi-producer queue used between the gateway, engine, and
// feed threads. Deliberately simple; the interface allows a lock-free ring
// swap-in later without touching callers. The optional notify hook lets a
// kqueue-based consumer be woken via a pipe write.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace nsq {

template <typename T>
class MpscQueue {
 public:
  void push(T v) {
    {
      std::lock_guard lk(mu_);
      items_.push_back(std::move(v));
    }
    cv_.notify_one();
    if (notify_) notify_();
  }

  // Non-blocking: returns everything queued so far.
  std::vector<T> drain() {
    std::lock_guard lk(mu_);
    return std::exchange(items_, {});
  }

  // Blocks until items are available or stop() is called; returns empty
  // only when stopped.
  std::vector<T> wait_drain() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [&] { return !items_.empty() || stopped_; });
    return std::exchange(items_, {});
  }

  // Like wait_drain but returns (possibly empty) after `timeout`; use
  // stopped() to distinguish shutdown from an idle tick.
  template <typename Rep, typename Period>
  std::vector<T> wait_drain_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lk(mu_);
    cv_.wait_for(lk, timeout, [&] { return !items_.empty() || stopped_; });
    return std::exchange(items_, {});
  }

  void stop() {
    {
      std::lock_guard lk(mu_);
      stopped_ = true;
    }
    cv_.notify_all();
  }

  bool stopped() {
    std::lock_guard lk(mu_);
    return stopped_;
  }

  // Called after every push, outside the lock. Set before threads start.
  void set_notify(std::function<void()> fn) { notify_ = std::move(fn); }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<T> items_;
  bool stopped_ = false;
  std::function<void()> notify_;
};

}  // namespace nsq

// The matching engine service: consumes decoded OUCH commands from the
// gateway, drives per-symbol reference books, and emits OUCH-shaped
// responses (to the gateway) plus protocol-neutral market events (to the
// feed). Single-writer: only the engine thread touches the books.
//
// process() is public and synchronous so tests can drive the engine without
// threads; start()/stop() wrap it in a queue-pumping thread.
#pragma once

#include "book/fast_book.hpp"
#include "common/cpu.hpp"
#include "common/queue.hpp"
#include "common/spsc_ring.hpp"
#include "common/types.hpp"
#include "ouch/ouch.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

namespace nsq::engine {

using ::nsq::MpscQueue;

struct Command {
  std::uint64_t client_id;
  std::variant<ouch::EnterOrder, ouch::CancelOrder, ouch::ReplaceOrder> msg;
};

// The gateway->engine hop. One producer (gateway) and one consumer (engine),
// so both backings are SPSC-correct. Default is the blocking mutex queue
// (a cv wakeup per push); low-latency mode swaps in the lock-free SPSC ring
// that the engine busy-polls — no lock for the spinning consumer to contend.
class CommandChannel {
 public:
  explicit CommandChannel(bool lock_free = false) : lock_free_(lock_free) {
    if (lock_free_) ring_ = std::make_unique<Ring>();
  }

  bool lock_free() const { return lock_free_; }

  // Producer side (gateway thread).
  void push(Command c) {
    if (lock_free_) {
      while (!ring_->try_push(std::move(c))) cpu_relax();  // ring full: backoff
    } else {
      mq_.push(std::move(c));
    }
  }

  // Consumer side (engine thread). try_pop for the busy-poll path,
  // wait_drain for the blocking path.
  std::optional<Command> try_pop() { return ring_->try_pop(); }
  std::vector<Command> wait_drain() { return mq_.wait_drain(); }

  void stop() {
    if (lock_free_) {
      stopped_.store(true, std::memory_order_release);
    } else {
      mq_.stop();
    }
  }
  bool stopped() const { return stopped_.load(std::memory_order_acquire); }

 private:
  using Ring = SpscRing<Command, (1u << 16)>;
  bool lock_free_;
  MpscQueue<Command> mq_;
  std::unique_ptr<Ring> ring_;
  std::atomic<bool> stopped_{false};
};

struct ClientResponse {
  std::uint64_t client_id;
  std::variant<ouch::SystemEvent, ouch::Accepted, ouch::Executed,
               ouch::Canceled, ouch::Replaced, ouch::Rejected>
      msg;
};

// Book event plus the context the feed needs to build ITCH messages.
struct MarketEvent {
  std::uint64_t ts;
  Symbol symbol;
  std::variant<AddedEvent, ExecutedEvent, CanceledEvent, ReplacedEvent> ev;
};

// Engine-thread tuning. Whether it busy-polls vs blocks is decided by the
// CommandChannel's backing (lock-free ring => busy-poll); these knobs are the
// jitter controls that make busy-polling worthwhile.
struct RunConfig {
  // Pin the engine thread to this core (>= 0). Real on Linux only.
  int pin_core = -1;
  // mlockall the process so the hot path never takes a page fault.
  bool lock_memory = false;
};

class Engine : private BookListener {
 public:
  Engine(CommandChannel& in, MpscQueue<ClientResponse>& to_gateway,
         MpscQueue<MarketEvent>& to_feed)
      : in_(in), to_gateway_(to_gateway), to_feed_(to_feed) {}
  ~Engine() { stop(); }

  void process(const Command& cmd);

  void start(RunConfig cfg = {});
  void stop();

  // Snapshot of one symbol's book. Only safe when the engine thread is not
  // running (tests / post-shutdown inspection).
  BookSnapshot book_snapshot(const Symbol& symbol) const;

 private:
  struct OrderInfo {
    std::uint64_t client_id;
    ouch::Token token;
    Symbol symbol;
    Side side;
    Qty open;
  };

  void run_loop();

  void handle(std::uint64_t client, const ouch::EnterOrder& m);
  void handle(std::uint64_t client, const ouch::CancelOrder& m);
  void handle(std::uint64_t client, const ouch::ReplaceOrder& m);

  // BookListener — called synchronously during book operations.
  void on_added(const AddedEvent& e) override;
  void on_executed(const ExecutedEvent& e) override;
  void on_canceled(const CanceledEvent& e) override;
  void on_replaced(const ReplacedEvent& e) override;

  FastBook& book_for(const Symbol& symbol);
  void reject(std::uint64_t client, const ouch::Token& token, char reason);
  // Send an OUCH Canceled for shares that never rested (IOC remainder or a
  // min-quantity kill) and drop the order's engine-side bookkeeping.
  void cancel_unrested(std::uint64_t client, OrderId ref,
                       const ouch::Token& token, Qty qty, char reason);
  bool token_used(std::uint64_t client, const ouch::Token& token) const;
  void reduce_open(OrderId id, Qty by);

  CommandChannel& in_;
  MpscQueue<ClientResponse>& to_gateway_;
  MpscQueue<MarketEvent>& to_feed_;

  std::map<Symbol, std::unique_ptr<FastBook>> books_;
  std::unordered_map<OrderId, OrderInfo> orders_;
  // (client, token) -> live order ref; used set persists for the whole day.
  std::map<std::pair<std::uint64_t, std::string>, OrderId> live_tokens_;
  std::set<std::pair<std::uint64_t, std::string>> used_tokens_;

  OrderId next_ref_ = 1;
  Symbol current_symbol_{};
  std::uint64_t now_ = 0;
  bool suppress_ouch_cancel_ = false;  // set while a replace crosses

  RunConfig cfg_;
  std::thread thread_;
};

}  // namespace nsq::engine

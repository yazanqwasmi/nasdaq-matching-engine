// The matching engine service: consumes decoded OUCH commands from the
// gateway, drives per-symbol reference books, and emits OUCH-shaped
// responses (to the gateway) plus protocol-neutral market events (to the
// feed). Single-writer: only the engine thread touches the books.
//
// process() is public and synchronous so tests can drive the engine without
// threads; start()/stop() wrap it in a queue-pumping thread.
#pragma once

#include "book/order_book.hpp"
#include "common/queue.hpp"
#include "common/types.hpp"
#include "ouch/ouch.hpp"

#include <cstdint>
#include <map>
#include <memory>
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

class Engine : private BookListener {
 public:
  Engine(MpscQueue<Command>& in, MpscQueue<ClientResponse>& to_gateway,
         MpscQueue<MarketEvent>& to_feed)
      : in_(in), to_gateway_(to_gateway), to_feed_(to_feed) {}
  ~Engine() { stop(); }

  void process(const Command& cmd);

  void start();
  void stop();

 private:
  struct OrderInfo {
    std::uint64_t client_id;
    ouch::Token token;
    Symbol symbol;
    Side side;
    Qty open;
  };

  void handle(std::uint64_t client, const ouch::EnterOrder& m);
  void handle(std::uint64_t client, const ouch::CancelOrder& m);
  void handle(std::uint64_t client, const ouch::ReplaceOrder& m);

  // BookListener — called synchronously during book operations.
  void on_added(const AddedEvent& e) override;
  void on_executed(const ExecutedEvent& e) override;
  void on_canceled(const CanceledEvent& e) override;
  void on_replaced(const ReplacedEvent& e) override;

  OrderBook& book_for(const Symbol& symbol);
  void reject(std::uint64_t client, const ouch::Token& token, char reason);
  bool token_used(std::uint64_t client, const ouch::Token& token) const;
  void reduce_open(OrderId id, Qty by);

  MpscQueue<Command>& in_;
  MpscQueue<ClientResponse>& to_gateway_;
  MpscQueue<MarketEvent>& to_feed_;

  std::map<Symbol, std::unique_ptr<OrderBook>> books_;
  std::unordered_map<OrderId, OrderInfo> orders_;
  // (client, token) -> live order ref; used set persists for the whole day.
  std::map<std::pair<std::uint64_t, std::string>, OrderId> live_tokens_;
  std::set<std::pair<std::uint64_t, std::string>> used_tokens_;

  OrderId next_ref_ = 1;
  Symbol current_symbol_{};
  std::uint64_t now_ = 0;
  bool suppress_ouch_cancel_ = false;  // set while a replace crosses

  std::thread thread_;
};

}  // namespace nsq::engine

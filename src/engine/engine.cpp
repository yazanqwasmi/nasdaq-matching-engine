#include "engine/engine.hpp"

#include "common/clock.hpp"

namespace nsq::engine {

namespace {
std::string token_str(const ouch::Token& t) {
  return std::string(ouch::token_view(t));
}
}  // namespace

void Engine::start() {
  thread_ = std::thread([this] {
    for (;;) {
      auto cmds = in_.wait_drain();
      if (cmds.empty()) return;  // stopped
      for (const auto& cmd : cmds) process(cmd);
    }
  });
}

void Engine::stop() {
  in_.stop();
  if (thread_.joinable()) thread_.join();
}

void Engine::process(const Command& cmd) {
  now_ = ns_since_midnight();
  std::visit([&](const auto& m) { handle(cmd.client_id, m); }, cmd.msg);
}

BookSnapshot Engine::book_snapshot(const Symbol& symbol) const {
  const auto it = books_.find(symbol);
  return it == books_.end() ? BookSnapshot{} : it->second->snapshot();
}

FastBook& Engine::book_for(const Symbol& symbol) {
  auto it = books_.find(symbol);
  if (it == books_.end())
    it = books_.emplace(symbol, std::make_unique<FastBook>(
                                    static_cast<BookListener&>(*this)))
             .first;
  return *it->second;
}

bool Engine::token_used(std::uint64_t client,
                        const ouch::Token& token) const {
  return used_tokens_.contains({client, token_str(token)});
}

void Engine::reject(std::uint64_t client, const ouch::Token& token,
                    char reason) {
  ouch::Rejected r{};
  r.timestamp = now_;
  r.token = token;
  r.reason = reason;
  to_gateway_.push({client, r});
}

void Engine::reduce_open(OrderId id, Qty by) {
  auto it = orders_.find(id);
  if (it == orders_.end()) return;
  it->second.open -= by;
  if (it->second.open == 0) {
    live_tokens_.erase({it->second.client_id, token_str(it->second.token)});
    orders_.erase(it);
  }
}

void Engine::handle(std::uint64_t client, const ouch::EnterOrder& m) {
  if (token_used(client, m.token)) return reject(client, m.token, 'T');
  if (m.price <= 0) return reject(client, m.token, 'X');
  if (m.shares == 0) return reject(client, m.token, 'Z');
  if (m.side != Side::Buy && m.side != Side::Sell)
    return reject(client, m.token, 'S');

  const OrderId ref = next_ref_++;
  used_tokens_.insert({client, token_str(m.token)});
  live_tokens_[{client, token_str(m.token)}] = ref;
  orders_[ref] = {client, m.token, m.stock, m.side, m.shares};
  current_symbol_ = m.stock;

  // NASDAQ sends Accepted before any executions for the order.
  ouch::Accepted acc{};
  acc.timestamp = now_;
  acc.token = m.token;
  acc.side = m.side;
  acc.shares = m.shares;
  acc.stock = m.stock;
  acc.price = m.price;
  acc.tif = m.tif;
  acc.firm = m.firm;
  acc.display = m.display;
  acc.order_ref = ref;
  acc.capacity = m.capacity;
  acc.intermarket_sweep = m.intermarket_sweep;
  acc.min_qty = m.min_qty;
  acc.cross_type = m.cross_type;
  acc.order_state = 'L';
  to_gateway_.push({client, acc});

  book_for(m.stock).add(ref, m.side, m.price, m.shares);
}

void Engine::handle(std::uint64_t client, const ouch::CancelOrder& m) {
  const auto it = live_tokens_.find({client, token_str(m.token)});
  if (it == live_tokens_.end()) return;  // unknown/foreign/dead: ignore
  const auto info = orders_.find(it->second);
  if (info == orders_.end()) return;
  current_symbol_ = info->second.symbol;
  book_for(info->second.symbol).cancel(it->second, m.shares);
}

void Engine::handle(std::uint64_t client, const ouch::ReplaceOrder& m) {
  const auto it = live_tokens_.find({client, token_str(m.existing_token)});
  if (it == live_tokens_.end()) return;
  if (token_used(client, m.replacement_token)) return;
  if (m.price <= 0 || m.shares == 0) return;

  const OrderId old_ref = it->second;
  const auto old_info = orders_.find(old_ref);
  if (old_info == orders_.end()) return;
  const Symbol symbol = old_info->second.symbol;
  const Side side = old_info->second.side;
  current_symbol_ = symbol;

  const OrderId new_ref = next_ref_++;
  used_tokens_.insert({client, token_str(m.replacement_token)});

  // Send Replaced before any executions the new price may trigger.
  ouch::Replaced rep{};
  rep.timestamp = now_;
  rep.replacement_token = m.replacement_token;
  rep.side = side;
  rep.shares = m.shares;
  rep.stock = symbol;
  rep.price = m.price;
  rep.tif = m.tif;
  rep.display = m.display;
  rep.order_ref = new_ref;
  rep.intermarket_sweep = m.intermarket_sweep;
  rep.min_qty = m.min_qty;
  rep.order_state = 'L';
  rep.previous_token = old_info->second.token;
  to_gateway_.push({client, rep});

  // Swap bookkeeping to the new order before the book emits events.
  live_tokens_.erase(it);
  orders_.erase(old_info);
  live_tokens_[{client, token_str(m.replacement_token)}] = new_ref;
  orders_[new_ref] = {client, m.replacement_token, symbol, side, m.shares};

  suppress_ouch_cancel_ = true;  // a crossing replace emits cancel(old)
  book_for(symbol).replace(old_ref, new_ref, m.price, m.shares);
  suppress_ouch_cancel_ = false;
}

void Engine::on_added(const AddedEvent& e) {
  to_feed_.push({now_, current_symbol_, e});
}

void Engine::on_executed(const ExecutedEvent& e) {
  to_feed_.push({now_, current_symbol_, e});

  // Fill notification to both owners: resting side added liquidity.
  const auto notify = [&](OrderId id, char liquidity) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) return;
    ouch::Executed ex{};
    ex.timestamp = now_;
    ex.token = it->second.token;
    ex.executed_shares = e.qty;
    ex.execution_price = e.price;
    ex.liquidity_flag = liquidity;
    ex.match_number = e.match_id;
    to_gateway_.push({it->second.client_id, ex});
  };
  notify(e.resting_id, 'A');
  notify(e.aggressor_id, 'R');

  reduce_open(e.resting_id, e.qty);
  reduce_open(e.aggressor_id, e.qty);
}

void Engine::on_canceled(const CanceledEvent& e) {
  to_feed_.push({now_, current_symbol_, e});

  if (!suppress_ouch_cancel_) {
    const auto it = orders_.find(e.id);
    if (it != orders_.end()) {
      ouch::Canceled c{};
      c.timestamp = now_;
      c.token = it->second.token;
      c.decrement_shares = e.canceled_qty;
      c.reason = 'U';
      to_gateway_.push({it->second.client_id, c});
    }
  }
  if (e.removed) {
    reduce_open(e.id, e.canceled_qty);
  } else {
    // partial reduce: order stays live
    const auto it = orders_.find(e.id);
    if (it != orders_.end()) it->second.open -= e.canceled_qty;
  }
}

void Engine::on_replaced(const ReplacedEvent& e) {
  to_feed_.push({now_, current_symbol_, e});
}

}  // namespace nsq::engine

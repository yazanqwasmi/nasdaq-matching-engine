// OUCH 4.2 message codec (documented subset).
//
// Inbound (client -> exchange): Enter Order 'O', Cancel Order 'X',
// Replace Order 'U'. Outbound (exchange -> client): System Event 'S',
// Accepted 'A', Executed 'E', Canceled 'C', Replaced 'U', Rejected 'J'.
//
// All integers big-endian; prices are 4-byte unsigned with 4 implied
// decimals (matching nsq::Price's scale); tokens are 14 chars space-padded.
// OUCH messages ride inside SoupBinTCP data packets and carry no framing
// of their own; decode functions take the exact message bytes.
#pragma once

#include "common/endian.hpp"
#include "common/types.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace nsq::ouch {

using Token = std::array<char, 14>;
using Firm = std::array<char, 4>;

inline Token make_token(std::string_view sv) {
  Token t;
  t.fill(' ');
  for (std::size_t i = 0; i < sv.size() && i < t.size(); ++i) t[i] = sv[i];
  return t;
}

inline std::string_view token_view(const Token& t) {
  std::size_t len = t.size();
  while (len > 0 && t[len - 1] == ' ') --len;
  return {t.data(), len};
}

namespace detail {

inline void put_chars(std::uint8_t* p, const char* s, std::size_t n) {
  std::memcpy(p, s, n);
}

inline void get_chars(const std::uint8_t* p, char* s, std::size_t n) {
  std::memcpy(s, p, n);
}

inline void put_price(std::uint8_t* p, Price v) {
  put_be32(p, static_cast<std::uint32_t>(v));
}

inline Price get_price(const std::uint8_t* p) {
  return static_cast<Price>(get_be32(p));
}

}  // namespace detail

// ---------------------------------------------------------------- inbound

struct EnterOrder {
  Token token;
  Side side;
  Qty shares;
  Symbol stock;
  Price price;
  std::uint32_t tif;  // seconds; 99999 = market hours
  Firm firm;
  char display;
  char capacity;
  char intermarket_sweep;
  Qty min_qty;
  char cross_type;
  char customer_type;
};
constexpr std::size_t kEnterOrderSize = 49;

inline void encode(const EnterOrder& m, std::uint8_t* p) {
  p[0] = 'O';
  detail::put_chars(p + 1, m.token.data(), 14);
  p[15] = static_cast<std::uint8_t>(m.side);
  put_be32(p + 16, m.shares);
  detail::put_chars(p + 20, m.stock.data(), 8);
  detail::put_price(p + 28, m.price);
  put_be32(p + 32, m.tif);
  detail::put_chars(p + 36, m.firm.data(), 4);
  p[40] = static_cast<std::uint8_t>(m.display);
  p[41] = static_cast<std::uint8_t>(m.capacity);
  p[42] = static_cast<std::uint8_t>(m.intermarket_sweep);
  put_be32(p + 43, m.min_qty);
  p[47] = static_cast<std::uint8_t>(m.cross_type);
  p[48] = static_cast<std::uint8_t>(m.customer_type);
}

inline std::optional<EnterOrder> decode_enter_order(const std::uint8_t* p,
                                                    std::size_t n) {
  if (n != kEnterOrderSize || p[0] != 'O') return std::nullopt;
  EnterOrder m{};
  detail::get_chars(p + 1, m.token.data(), 14);
  m.side = static_cast<Side>(p[15]);
  m.shares = get_be32(p + 16);
  detail::get_chars(p + 20, m.stock.data(), 8);
  m.price = detail::get_price(p + 28);
  m.tif = get_be32(p + 32);
  detail::get_chars(p + 36, m.firm.data(), 4);
  m.display = static_cast<char>(p[40]);
  m.capacity = static_cast<char>(p[41]);
  m.intermarket_sweep = static_cast<char>(p[42]);
  m.min_qty = get_be32(p + 43);
  m.cross_type = static_cast<char>(p[47]);
  m.customer_type = static_cast<char>(p[48]);
  return m;
}

struct CancelOrder {
  Token token;
  Qty shares;  // intended remaining open shares; 0 = full cancel
};
constexpr std::size_t kCancelOrderSize = 19;

inline void encode(const CancelOrder& m, std::uint8_t* p) {
  p[0] = 'X';
  detail::put_chars(p + 1, m.token.data(), 14);
  put_be32(p + 15, m.shares);
}

inline std::optional<CancelOrder> decode_cancel_order(const std::uint8_t* p,
                                                      std::size_t n) {
  if (n != kCancelOrderSize || p[0] != 'X') return std::nullopt;
  CancelOrder m{};
  detail::get_chars(p + 1, m.token.data(), 14);
  m.shares = get_be32(p + 15);
  return m;
}

struct ReplaceOrder {
  Token existing_token;
  Token replacement_token;
  Qty shares;
  Price price;
  std::uint32_t tif;
  char display;
  char intermarket_sweep;
  Qty min_qty;
};
constexpr std::size_t kReplaceOrderSize = 47;

inline void encode(const ReplaceOrder& m, std::uint8_t* p) {
  p[0] = 'U';
  detail::put_chars(p + 1, m.existing_token.data(), 14);
  detail::put_chars(p + 15, m.replacement_token.data(), 14);
  put_be32(p + 29, m.shares);
  detail::put_price(p + 33, m.price);
  put_be32(p + 37, m.tif);
  p[41] = static_cast<std::uint8_t>(m.display);
  p[42] = static_cast<std::uint8_t>(m.intermarket_sweep);
  put_be32(p + 43, m.min_qty);
}

inline std::optional<ReplaceOrder> decode_replace_order(const std::uint8_t* p,
                                                        std::size_t n) {
  if (n != kReplaceOrderSize || p[0] != 'U') return std::nullopt;
  ReplaceOrder m{};
  detail::get_chars(p + 1, m.existing_token.data(), 14);
  detail::get_chars(p + 15, m.replacement_token.data(), 14);
  m.shares = get_be32(p + 29);
  m.price = detail::get_price(p + 33);
  m.tif = get_be32(p + 37);
  m.display = static_cast<char>(p[41]);
  m.intermarket_sweep = static_cast<char>(p[42]);
  m.min_qty = get_be32(p + 43);
  return m;
}

// --------------------------------------------------------------- outbound

struct SystemEvent {
  std::uint64_t timestamp;
  char event_code;  // 'S' start of day, 'E' end of day
};
constexpr std::size_t kSystemEventSize = 10;

inline void encode(const SystemEvent& m, std::uint8_t* p) {
  p[0] = 'S';
  put_be64(p + 1, m.timestamp);
  p[9] = static_cast<std::uint8_t>(m.event_code);
}

inline std::optional<SystemEvent> decode_system_event(const std::uint8_t* p,
                                                      std::size_t n) {
  if (n != kSystemEventSize || p[0] != 'S') return std::nullopt;
  SystemEvent m{};
  m.timestamp = get_be64(p + 1);
  m.event_code = static_cast<char>(p[9]);
  return m;
}

struct Accepted {
  std::uint64_t timestamp;
  Token token;
  Side side;
  Qty shares;
  Symbol stock;
  Price price;
  std::uint32_t tif;
  Firm firm;
  char display;
  std::uint64_t order_ref;
  char capacity;
  char intermarket_sweep;
  Qty min_qty;
  char cross_type;
  char order_state;  // 'L' live, 'D' dead
  char bbo_weight_indicator;
};
constexpr std::size_t kAcceptedSize = 66;

namespace detail {
// Accepted and Replaced share this 66-byte body (type byte differs).
inline void put_accepted_body(const Accepted& m, std::uint8_t* p) {
  put_be64(p + 1, m.timestamp);
  put_chars(p + 9, m.token.data(), 14);
  p[23] = static_cast<std::uint8_t>(m.side);
  put_be32(p + 24, m.shares);
  put_chars(p + 28, m.stock.data(), 8);
  put_price(p + 36, m.price);
  put_be32(p + 40, m.tif);
  put_chars(p + 44, m.firm.data(), 4);
  p[48] = static_cast<std::uint8_t>(m.display);
  put_be64(p + 49, m.order_ref);
  p[57] = static_cast<std::uint8_t>(m.capacity);
  p[58] = static_cast<std::uint8_t>(m.intermarket_sweep);
  put_be32(p + 59, m.min_qty);
  p[63] = static_cast<std::uint8_t>(m.cross_type);
  p[64] = static_cast<std::uint8_t>(m.order_state);
  p[65] = static_cast<std::uint8_t>(m.bbo_weight_indicator);
}

inline Accepted get_accepted_body(const std::uint8_t* p) {
  Accepted m{};
  m.timestamp = get_be64(p + 1);
  get_chars(p + 9, m.token.data(), 14);
  m.side = static_cast<Side>(p[23]);
  m.shares = get_be32(p + 24);
  get_chars(p + 28, m.stock.data(), 8);
  m.price = get_price(p + 36);
  m.tif = get_be32(p + 40);
  get_chars(p + 44, m.firm.data(), 4);
  m.display = static_cast<char>(p[48]);
  m.order_ref = get_be64(p + 49);
  m.capacity = static_cast<char>(p[57]);
  m.intermarket_sweep = static_cast<char>(p[58]);
  m.min_qty = get_be32(p + 59);
  m.cross_type = static_cast<char>(p[63]);
  m.order_state = static_cast<char>(p[64]);
  m.bbo_weight_indicator = static_cast<char>(p[65]);
  return m;
}
}  // namespace detail

inline void encode(const Accepted& m, std::uint8_t* p) {
  p[0] = 'A';
  detail::put_accepted_body(m, p);
}

inline std::optional<Accepted> decode_accepted(const std::uint8_t* p,
                                               std::size_t n) {
  if (n != kAcceptedSize || p[0] != 'A') return std::nullopt;
  return detail::get_accepted_body(p);
}

struct Replaced {
  std::uint64_t timestamp;
  Token replacement_token;
  Side side;
  Qty shares;
  Symbol stock;
  Price price;
  std::uint32_t tif;
  Firm firm;
  char display;
  std::uint64_t order_ref;
  char capacity;
  char intermarket_sweep;
  Qty min_qty;
  char cross_type;
  char order_state;
  char bbo_weight_indicator;
  Token previous_token;
};
constexpr std::size_t kReplacedSize = 80;

inline void encode(const Replaced& m, std::uint8_t* p) {
  p[0] = 'U';
  Accepted body{m.timestamp, m.replacement_token, m.side,
                m.shares,    m.stock,             m.price,
                m.tif,       m.firm,              m.display,
                m.order_ref, m.capacity,          m.intermarket_sweep,
                m.min_qty,   m.cross_type,        m.order_state,
                m.bbo_weight_indicator};
  detail::put_accepted_body(body, p);
  detail::put_chars(p + 66, m.previous_token.data(), 14);
}

inline std::optional<Replaced> decode_replaced(const std::uint8_t* p,
                                               std::size_t n) {
  if (n != kReplacedSize || p[0] != 'U') return std::nullopt;
  const Accepted body = detail::get_accepted_body(p);
  Replaced m{body.timestamp, body.token,     body.side,
             body.shares,    body.stock,     body.price,
             body.tif,       body.firm,      body.display,
             body.order_ref, body.capacity,  body.intermarket_sweep,
             body.min_qty,   body.cross_type, body.order_state,
             body.bbo_weight_indicator,       {}};
  detail::get_chars(p + 66, m.previous_token.data(), 14);
  return m;
}

struct Executed {
  std::uint64_t timestamp;
  Token token;
  Qty executed_shares;
  Price execution_price;
  char liquidity_flag;  // 'A' added, 'R' removed
  std::uint64_t match_number;
};
constexpr std::size_t kExecutedSize = 40;

inline void encode(const Executed& m, std::uint8_t* p) {
  p[0] = 'E';
  put_be64(p + 1, m.timestamp);
  detail::put_chars(p + 9, m.token.data(), 14);
  put_be32(p + 23, m.executed_shares);
  detail::put_price(p + 27, m.execution_price);
  p[31] = static_cast<std::uint8_t>(m.liquidity_flag);
  put_be64(p + 32, m.match_number);
}

inline std::optional<Executed> decode_executed(const std::uint8_t* p,
                                               std::size_t n) {
  if (n != kExecutedSize || p[0] != 'E') return std::nullopt;
  Executed m{};
  m.timestamp = get_be64(p + 1);
  detail::get_chars(p + 9, m.token.data(), 14);
  m.executed_shares = get_be32(p + 23);
  m.execution_price = detail::get_price(p + 27);
  m.liquidity_flag = static_cast<char>(p[31]);
  m.match_number = get_be64(p + 32);
  return m;
}

struct Canceled {
  std::uint64_t timestamp;
  Token token;
  Qty decrement_shares;
  char reason;  // 'U' user requested
};
constexpr std::size_t kCanceledSize = 28;

inline void encode(const Canceled& m, std::uint8_t* p) {
  p[0] = 'C';
  put_be64(p + 1, m.timestamp);
  detail::put_chars(p + 9, m.token.data(), 14);
  put_be32(p + 23, m.decrement_shares);
  p[27] = static_cast<std::uint8_t>(m.reason);
}

inline std::optional<Canceled> decode_canceled(const std::uint8_t* p,
                                               std::size_t n) {
  if (n != kCanceledSize || p[0] != 'C') return std::nullopt;
  Canceled m{};
  m.timestamp = get_be64(p + 1);
  detail::get_chars(p + 9, m.token.data(), 14);
  m.decrement_shares = get_be32(p + 23);
  m.reason = static_cast<char>(p[27]);
  return m;
}

struct Rejected {
  std::uint64_t timestamp;
  Token token;
  char reason;
};
constexpr std::size_t kRejectedSize = 24;

inline void encode(const Rejected& m, std::uint8_t* p) {
  p[0] = 'J';
  put_be64(p + 1, m.timestamp);
  detail::put_chars(p + 9, m.token.data(), 14);
  p[23] = static_cast<std::uint8_t>(m.reason);
}

inline std::optional<Rejected> decode_rejected(const std::uint8_t* p,
                                               std::size_t n) {
  if (n != kRejectedSize || p[0] != 'J') return std::nullopt;
  Rejected m{};
  m.timestamp = get_be64(p + 1);
  detail::get_chars(p + 9, m.token.data(), 14);
  m.reason = static_cast<char>(p[23]);
  return m;
}

}  // namespace nsq::ouch

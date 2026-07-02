// ITCH 5.0 message codec (documented subset).
//
// Every message starts with: type(1), stock locate(2), tracking number(2),
// timestamp(6, ns since midnight). All integers big-endian; prices are
// 4-byte with 4 implied decimals. Messages carry no framing of their own —
// they ride inside MoldUDP64 message blocks.
//
// Subset: SystemEvent 'S', StockDirectory 'R', TradingAction 'H',
// AddOrder 'A', AddOrderMpid 'F', OrderExecuted 'E',
// OrderExecutedWithPrice 'C', OrderCancel 'X', OrderDelete 'D',
// OrderReplace 'U', Trade 'P'. Cross/NOII/MWCB messages are out of scope.
#pragma once

#include "common/endian.hpp"
#include "common/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <variant>

namespace nsq::itch {

struct Header {
  std::uint16_t stock_locate;
  std::uint16_t tracking;
  std::uint64_t timestamp;  // low 48 bits on the wire
};

namespace detail {

inline void put_header(std::uint8_t* p, char type, const Header& h) {
  p[0] = static_cast<std::uint8_t>(type);
  put_be16(p + 1, h.stock_locate);
  put_be16(p + 3, h.tracking);
  put_be48(p + 5, h.timestamp);
}

inline void get_header(const std::uint8_t* p, Header& h) {
  h.stock_locate = get_be16(p + 1);
  h.tracking = get_be16(p + 3);
  h.timestamp = get_be48(p + 5);
}

inline void put_price(std::uint8_t* p, Price v) {
  put_be32(p, static_cast<std::uint32_t>(v));
}

inline Price get_price(const std::uint8_t* p) {
  return static_cast<Price>(get_be32(p));
}

inline void put_symbol(std::uint8_t* p, const Symbol& s) {
  for (std::size_t i = 0; i < s.size(); ++i)
    p[i] = static_cast<std::uint8_t>(s[i]);
}

inline void get_symbol(const std::uint8_t* p, Symbol& s) {
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(p[i]);
}

}  // namespace detail

struct SystemEvent : Header {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 12;
  char event_code;  // 'O','S','Q','M','E','C'
};

struct StockDirectory : Header {
  static constexpr char kType = 'R';
  static constexpr std::size_t kSize = 39;
  Symbol stock;
  char market_category;
  char financial_status;
  std::uint32_t round_lot_size;
  char round_lots_only;
  char issue_classification;
  std::array<char, 2> issue_subtype;
  char authenticity;
  char short_sale_threshold;
  char ipo_flag;
  char luld_tier;
  char etp_flag;
  std::uint32_t etp_leverage;
  char inverse_indicator;
};

struct TradingAction : Header {
  static constexpr char kType = 'H';
  static constexpr std::size_t kSize = 25;
  Symbol stock;
  char trading_state;  // 'H' halted, 'P' paused, 'Q' quote only, 'T' trading
  char reserved;
  std::array<char, 4> reason;
};

struct AddOrder : Header {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 36;
  std::uint64_t order_ref;
  Side side;
  Qty shares;
  Symbol stock;
  Price price;
};

struct AddOrderMpid {
  static constexpr char kType = 'F';
  static constexpr std::size_t kSize = 40;
  AddOrder add;
  std::array<char, 4> attribution;
};

struct OrderExecuted : Header {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 31;
  std::uint64_t order_ref;
  Qty executed_shares;
  std::uint64_t match_number;
};

struct OrderExecutedWithPrice {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 36;
  OrderExecuted exec;
  char printable;
  Price execution_price;
};

struct OrderCancel : Header {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 23;
  std::uint64_t order_ref;
  Qty canceled_shares;
};

struct OrderDelete : Header {
  static constexpr char kType = 'D';
  static constexpr std::size_t kSize = 19;
  std::uint64_t order_ref;
};

struct OrderReplace : Header {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 35;
  std::uint64_t original_ref;
  std::uint64_t new_ref;
  Qty shares;
  Price price;
};

struct Trade : Header {
  static constexpr char kType = 'P';
  static constexpr std::size_t kSize = 44;
  std::uint64_t order_ref;  // 0 for non-displayable executions
  Side side;
  Qty shares;
  Symbol stock;
  Price price;
  std::uint64_t match_number;
};

// ------------------------------------------------------------------ encode

inline void encode(const SystemEvent& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  p[11] = static_cast<std::uint8_t>(m.event_code);
}

inline void encode(const StockDirectory& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  detail::put_symbol(p + 11, m.stock);
  p[19] = static_cast<std::uint8_t>(m.market_category);
  p[20] = static_cast<std::uint8_t>(m.financial_status);
  put_be32(p + 21, m.round_lot_size);
  p[25] = static_cast<std::uint8_t>(m.round_lots_only);
  p[26] = static_cast<std::uint8_t>(m.issue_classification);
  p[27] = static_cast<std::uint8_t>(m.issue_subtype[0]);
  p[28] = static_cast<std::uint8_t>(m.issue_subtype[1]);
  p[29] = static_cast<std::uint8_t>(m.authenticity);
  p[30] = static_cast<std::uint8_t>(m.short_sale_threshold);
  p[31] = static_cast<std::uint8_t>(m.ipo_flag);
  p[32] = static_cast<std::uint8_t>(m.luld_tier);
  p[33] = static_cast<std::uint8_t>(m.etp_flag);
  put_be32(p + 34, m.etp_leverage);
  p[38] = static_cast<std::uint8_t>(m.inverse_indicator);
}

inline void encode(const TradingAction& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  detail::put_symbol(p + 11, m.stock);
  p[19] = static_cast<std::uint8_t>(m.trading_state);
  p[20] = static_cast<std::uint8_t>(m.reserved);
  for (std::size_t i = 0; i < 4; ++i)
    p[21 + i] = static_cast<std::uint8_t>(m.reason[i]);
}

inline void encode(const AddOrder& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.order_ref);
  p[19] = static_cast<std::uint8_t>(m.side);
  put_be32(p + 20, m.shares);
  detail::put_symbol(p + 24, m.stock);
  detail::put_price(p + 32, m.price);
}

inline void encode(const AddOrderMpid& m, std::uint8_t* p) {
  encode(m.add, p);
  p[0] = static_cast<std::uint8_t>(m.kType);
  for (std::size_t i = 0; i < 4; ++i)
    p[36 + i] = static_cast<std::uint8_t>(m.attribution[i]);
}

inline void encode(const OrderExecuted& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.order_ref);
  put_be32(p + 19, m.executed_shares);
  put_be64(p + 23, m.match_number);
}

inline void encode(const OrderExecutedWithPrice& m, std::uint8_t* p) {
  encode(m.exec, p);
  p[0] = static_cast<std::uint8_t>(m.kType);
  p[31] = static_cast<std::uint8_t>(m.printable);
  detail::put_price(p + 32, m.execution_price);
}

inline void encode(const OrderCancel& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.order_ref);
  put_be32(p + 19, m.canceled_shares);
}

inline void encode(const OrderDelete& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.order_ref);
}

inline void encode(const OrderReplace& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.original_ref);
  put_be64(p + 19, m.new_ref);
  put_be32(p + 27, m.shares);
  detail::put_price(p + 31, m.price);
}

inline void encode(const Trade& m, std::uint8_t* p) {
  detail::put_header(p, m.kType, m);
  put_be64(p + 11, m.order_ref);
  p[19] = static_cast<std::uint8_t>(m.side);
  put_be32(p + 20, m.shares);
  detail::put_symbol(p + 24, m.stock);
  detail::put_price(p + 32, m.price);
  put_be64(p + 40, m.match_number);
}

// ------------------------------------------------------------------ decode

using Message =
    std::variant<SystemEvent, StockDirectory, TradingAction, AddOrder,
                 AddOrderMpid, OrderExecuted, OrderExecutedWithPrice,
                 OrderCancel, OrderDelete, OrderReplace, Trade>;

inline std::optional<Message> decode(const std::uint8_t* p, std::size_t n) {
  if (n == 0) return std::nullopt;
  switch (static_cast<char>(p[0])) {
    case SystemEvent::kType: {
      if (n != SystemEvent::kSize) return std::nullopt;
      SystemEvent m{};
      detail::get_header(p, m);
      m.event_code = static_cast<char>(p[11]);
      return m;
    }
    case StockDirectory::kType: {
      if (n != StockDirectory::kSize) return std::nullopt;
      StockDirectory m{};
      detail::get_header(p, m);
      detail::get_symbol(p + 11, m.stock);
      m.market_category = static_cast<char>(p[19]);
      m.financial_status = static_cast<char>(p[20]);
      m.round_lot_size = get_be32(p + 21);
      m.round_lots_only = static_cast<char>(p[25]);
      m.issue_classification = static_cast<char>(p[26]);
      m.issue_subtype = {static_cast<char>(p[27]), static_cast<char>(p[28])};
      m.authenticity = static_cast<char>(p[29]);
      m.short_sale_threshold = static_cast<char>(p[30]);
      m.ipo_flag = static_cast<char>(p[31]);
      m.luld_tier = static_cast<char>(p[32]);
      m.etp_flag = static_cast<char>(p[33]);
      m.etp_leverage = get_be32(p + 34);
      m.inverse_indicator = static_cast<char>(p[38]);
      return m;
    }
    case TradingAction::kType: {
      if (n != TradingAction::kSize) return std::nullopt;
      TradingAction m{};
      detail::get_header(p, m);
      detail::get_symbol(p + 11, m.stock);
      m.trading_state = static_cast<char>(p[19]);
      m.reserved = static_cast<char>(p[20]);
      for (std::size_t i = 0; i < 4; ++i)
        m.reason[i] = static_cast<char>(p[21 + i]);
      return m;
    }
    case AddOrder::kType: {
      if (n != AddOrder::kSize) return std::nullopt;
      AddOrder m{};
      detail::get_header(p, m);
      m.order_ref = get_be64(p + 11);
      m.side = static_cast<Side>(p[19]);
      m.shares = get_be32(p + 20);
      detail::get_symbol(p + 24, m.stock);
      m.price = detail::get_price(p + 32);
      return m;
    }
    case AddOrderMpid::kType: {
      if (n != AddOrderMpid::kSize) return std::nullopt;
      AddOrderMpid m{};
      detail::get_header(p, m.add);
      m.add.order_ref = get_be64(p + 11);
      m.add.side = static_cast<Side>(p[19]);
      m.add.shares = get_be32(p + 20);
      detail::get_symbol(p + 24, m.add.stock);
      m.add.price = detail::get_price(p + 32);
      for (std::size_t i = 0; i < 4; ++i)
        m.attribution[i] = static_cast<char>(p[36 + i]);
      return m;
    }
    case OrderExecuted::kType: {
      if (n != OrderExecuted::kSize) return std::nullopt;
      OrderExecuted m{};
      detail::get_header(p, m);
      m.order_ref = get_be64(p + 11);
      m.executed_shares = get_be32(p + 19);
      m.match_number = get_be64(p + 23);
      return m;
    }
    case OrderExecutedWithPrice::kType: {
      if (n != OrderExecutedWithPrice::kSize) return std::nullopt;
      OrderExecutedWithPrice m{};
      detail::get_header(p, m.exec);
      m.exec.order_ref = get_be64(p + 11);
      m.exec.executed_shares = get_be32(p + 19);
      m.exec.match_number = get_be64(p + 23);
      m.printable = static_cast<char>(p[31]);
      m.execution_price = detail::get_price(p + 32);
      return m;
    }
    case OrderCancel::kType: {
      if (n != OrderCancel::kSize) return std::nullopt;
      OrderCancel m{};
      detail::get_header(p, m);
      m.order_ref = get_be64(p + 11);
      m.canceled_shares = get_be32(p + 19);
      return m;
    }
    case OrderDelete::kType: {
      if (n != OrderDelete::kSize) return std::nullopt;
      OrderDelete m{};
      detail::get_header(p, m);
      m.order_ref = get_be64(p + 11);
      return m;
    }
    case OrderReplace::kType: {
      if (n != OrderReplace::kSize) return std::nullopt;
      OrderReplace m{};
      detail::get_header(p, m);
      m.original_ref = get_be64(p + 11);
      m.new_ref = get_be64(p + 19);
      m.shares = get_be32(p + 27);
      m.price = detail::get_price(p + 31);
      return m;
    }
    case Trade::kType: {
      if (n != Trade::kSize) return std::nullopt;
      Trade m{};
      detail::get_header(p, m);
      m.order_ref = get_be64(p + 11);
      m.side = static_cast<Side>(p[19]);
      m.shares = get_be32(p + 20);
      detail::get_symbol(p + 24, m.stock);
      m.price = detail::get_price(p + 32);
      m.match_number = get_be64(p + 40);
      return m;
    }
    default:
      return std::nullopt;
  }
}

// Total wire size for every ITCH 5.0 message type (including types outside
// the decoded subset, so raw-file readers can skip them without losing
// framing). Returns 0 for unknown types.
constexpr std::size_t message_size(char type) {
  switch (type) {
    case 'S': return SystemEvent::kSize;
    case 'R': return StockDirectory::kSize;
    case 'H': return TradingAction::kSize;
    case 'A': return AddOrder::kSize;
    case 'F': return AddOrderMpid::kSize;
    case 'E': return OrderExecuted::kSize;
    case 'C': return OrderExecutedWithPrice::kSize;
    case 'X': return OrderCancel::kSize;
    case 'D': return OrderDelete::kSize;
    case 'U': return OrderReplace::kSize;
    case 'P': return Trade::kSize;
    case 'Y': return 20;  // Reg SHO short sale price test restriction
    case 'L': return 26;  // market participant position
    case 'V': return 35;  // MWCB decline levels
    case 'W': return 12;  // MWCB status
    case 'K': return 28;  // IPO quoting period update
    case 'J': return 35;  // LULD auction collar
    case 'h': return 21;  // operational halt
    case 'Q': return 40;  // cross trade
    case 'B': return 19;  // broken trade
    case 'I': return 50;  // net order imbalance indicator
    case 'N': return 20;  // retail price improvement indicator
    case 'O': return 48;  // direct listing with capital raise
    default: return 0;
  }
}

}  // namespace nsq::itch

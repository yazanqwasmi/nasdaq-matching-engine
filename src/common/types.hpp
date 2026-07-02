// Core domain types shared by the matching engine and all protocol codecs.
// Prices are fixed-point int64 in units of 1/10000 dollar (never float).
// Symbols are 8 chars, right-padded with spaces, per NASDAQ convention.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace nsq {

using Price = std::int64_t;    // dollars * 10000
using Qty = std::uint32_t;     // shares
using OrderId = std::uint64_t;

enum class Side : char { Buy = 'B', Sell = 'S' };

constexpr Price kPriceScale = 10000;

constexpr Price make_price(std::int64_t dollars, std::int64_t tenthousandths) {
  return dollars * kPriceScale + tenthousandths;
}

inline std::string price_to_string(Price p) {
  std::string s = std::to_string(p / kPriceScale);
  s += '.';
  std::string frac = std::to_string(p % kPriceScale);
  s.append(4 - frac.size(), '0');
  s += frac;
  return s;
}

using Symbol = std::array<char, 8>;

inline Symbol make_symbol(std::string_view sv) {
  Symbol s{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  for (std::size_t i = 0; i < sv.size() && i < s.size(); ++i) s[i] = sv[i];
  return s;
}

inline std::string_view symbol_view(const Symbol& s) {
  std::size_t len = s.size();
  while (len > 0 && s[len - 1] == ' ') --len;
  return {s.data(), len};
}

}  // namespace nsq

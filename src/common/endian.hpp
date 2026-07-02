// Big-endian byte-order helpers. All NASDAQ wire protocols (OUCH, ITCH,
// SoupBinTCP, MoldUDP64) are big-endian; these are the only byte-level
// primitives the codecs use.
#pragma once

#include <cstdint>

namespace nsq {

inline void put_be16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}

inline std::uint16_t get_be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | std::uint16_t{p[1]});
}

inline void put_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

inline std::uint32_t get_be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
         (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

// 48-bit field: low 6 bytes of v; used for ITCH timestamps.
inline void put_be48(std::uint8_t* p, std::uint64_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 40);
  p[1] = static_cast<std::uint8_t>(v >> 32);
  p[2] = static_cast<std::uint8_t>(v >> 24);
  p[3] = static_cast<std::uint8_t>(v >> 16);
  p[4] = static_cast<std::uint8_t>(v >> 8);
  p[5] = static_cast<std::uint8_t>(v);
}

inline std::uint64_t get_be48(const std::uint8_t* p) {
  return (std::uint64_t{p[0]} << 40) | (std::uint64_t{p[1]} << 32) |
         (std::uint64_t{p[2]} << 24) | (std::uint64_t{p[3]} << 16) |
         (std::uint64_t{p[4]} << 8) | std::uint64_t{p[5]};
}

inline void put_be64(std::uint8_t* p, std::uint64_t v) {
  put_be32(p, static_cast<std::uint32_t>(v >> 32));
  put_be32(p + 4, static_cast<std::uint32_t>(v));
}

inline std::uint64_t get_be64(const std::uint8_t* p) {
  return (std::uint64_t{get_be32(p)} << 32) | std::uint64_t{get_be32(p + 4)};
}

}  // namespace nsq

// Reader for NASDAQ "raw ITCH" files (the format of the public
// TotalView-ITCH 5.0 sample days): a stream of messages, each prefixed by
// a 2-byte big-endian length. Streams from any FILE* (including stdin, so
// multi-GB gzipped files can be piped through gzcat without ever being
// materialized). A truncated tail ends the stream silently; a length that
// contradicts message_size() for a known type is a framing error reported
// with its byte offset.
#pragma once

#include "common/endian.hpp"
#include "itch/itch.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace nsq::itch {

inline void append_raw(std::vector<std::uint8_t>& out,
                       const std::uint8_t* msg, std::size_t n) {
  const std::size_t at = out.size();
  out.resize(at + 2);
  put_be16(out.data() + at, static_cast<std::uint16_t>(n));
  out.insert(out.end(), msg, msg + n);
}

class RawItchReader {
 public:
  explicit RawItchReader(std::FILE* f) : f_(f) {}

  std::optional<std::vector<std::uint8_t>> next() {
    std::uint8_t hdr[2];
    if (std::fread(hdr, 1, 2, f_) != 2) return std::nullopt;  // EOF
    const std::uint16_t len = get_be16(hdr);
    if (len == 0) {
      error_ = "zero-length message at offset " + std::to_string(offset_);
      return std::nullopt;
    }
    std::vector<std::uint8_t> msg(len);
    if (std::fread(msg.data(), 1, len, f_) != len)
      return std::nullopt;  // truncated tail: tolerate silently

    const std::size_t expect = message_size(static_cast<char>(msg[0]));
    if (expect != 0 && expect != len) {
      error_ = std::string("message type '") + static_cast<char>(msg[0]) +
               "' with length " + std::to_string(len) + " (expected " +
               std::to_string(expect) + ") at offset " +
               std::to_string(offset_);
      return std::nullopt;
    }
    offset_ += 2u + len;
    return msg;
  }

  const std::string& error() const { return error_; }
  std::uint64_t offset() const { return offset_; }

 private:
  std::FILE* f_;
  std::uint64_t offset_ = 0;
  std::string error_;
};

}  // namespace nsq::itch

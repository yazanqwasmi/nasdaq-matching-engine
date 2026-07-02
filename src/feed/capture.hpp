// Feed capture file format: "NSQCAP01" magic, then repeated records of
// big-endian uint64 capture timestamp (ns since midnight) + uint16 length +
// raw MoldUDP64 datagram bytes. Written by the feed's capture tee, read by
// itchreplay and tests. Truncated tails are tolerated (reader just stops).
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace nsq::feed {

inline constexpr char kCaptureMagic[8] = {'N', 'S', 'Q', 'C',
                                          'A', 'P', '0', '1'};

class CaptureWriter {
 public:
  explicit CaptureWriter(const std::string& path);
  ~CaptureWriter();
  CaptureWriter(const CaptureWriter&) = delete;
  CaptureWriter& operator=(const CaptureWriter&) = delete;

  bool ok() const { return f_ != nullptr; }
  void write(std::uint64_t ts_ns, const std::uint8_t* data, std::size_t n);

 private:
  std::FILE* f_ = nullptr;
};

struct CaptureRecord {
  std::uint64_t ts_ns;
  std::vector<std::uint8_t> bytes;
};

class CaptureReader {
 public:
  explicit CaptureReader(const std::string& path);
  ~CaptureReader();
  CaptureReader(const CaptureReader&) = delete;
  CaptureReader& operator=(const CaptureReader&) = delete;

  bool ok() const { return f_ != nullptr; }
  std::optional<CaptureRecord> next();

 private:
  std::FILE* f_ = nullptr;
};

}  // namespace nsq::feed

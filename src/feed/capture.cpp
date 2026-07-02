#include "feed/capture.hpp"

#include "common/endian.hpp"

#include <cstring>

namespace nsq::feed {

CaptureWriter::CaptureWriter(const std::string& path) {
  f_ = std::fopen(path.c_str(), "wb");
  if (f_ != nullptr) std::fwrite(kCaptureMagic, 1, sizeof kCaptureMagic, f_);
}

CaptureWriter::~CaptureWriter() {
  if (f_ != nullptr) std::fclose(f_);
}

void CaptureWriter::write(std::uint64_t ts_ns, const std::uint8_t* data,
                          std::size_t n) {
  if (f_ == nullptr) return;
  std::uint8_t hdr[10];
  put_be64(hdr, ts_ns);
  put_be16(hdr + 8, static_cast<std::uint16_t>(n));
  std::fwrite(hdr, 1, sizeof hdr, f_);
  std::fwrite(data, 1, n, f_);
}

CaptureReader::CaptureReader(const std::string& path) {
  f_ = std::fopen(path.c_str(), "rb");
  if (f_ == nullptr) return;
  char magic[sizeof kCaptureMagic];
  if (std::fread(magic, 1, sizeof magic, f_) != sizeof magic ||
      std::memcmp(magic, kCaptureMagic, sizeof magic) != 0) {
    std::fclose(f_);
    f_ = nullptr;
  }
}

CaptureReader::~CaptureReader() {
  if (f_ != nullptr) std::fclose(f_);
}

std::optional<CaptureRecord> CaptureReader::next() {
  if (f_ == nullptr) return std::nullopt;
  std::uint8_t hdr[10];
  if (std::fread(hdr, 1, sizeof hdr, f_) != sizeof hdr) return std::nullopt;
  CaptureRecord rec;
  rec.ts_ns = get_be64(hdr);
  const std::uint16_t len = get_be16(hdr + 8);
  rec.bytes.resize(len);
  if (std::fread(rec.bytes.data(), 1, len, f_) != len) return std::nullopt;
  return rec;
}

}  // namespace nsq::feed

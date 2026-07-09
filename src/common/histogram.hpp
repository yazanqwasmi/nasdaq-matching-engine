// Log-linear latency histogram (HdrHistogram-style): 64 power-of-two major
// buckets, 16 linear minor buckets each => ~6.25% value resolution with a
// fixed 1024-slot footprint and O(1) record. Values are nanoseconds.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nsq {

class LatencyHistogram {
 public:
  void record(std::uint64_t ns) {
    ++buckets_[slot(ns)];
    ++count_;
    if (ns > max_) max_ = ns;
    if (ns < min_ || count_ == 1) min_ = ns;
  }

  std::uint64_t count() const { return count_; }
  std::uint64_t max() const { return max_; }
  std::uint64_t min() const { return count_ ? min_ : 0; }

  // Upper bound of the bucket containing the p-th percentile.
  std::uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(
        static_cast<double>(count_) * p / 100.0 + 0.5);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      seen += buckets_[i];
      if (seen >= target && buckets_[i] > 0) return upper_bound(i);
    }
    return max_;
  }

  std::string summary(const char* label) const {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "%-22s n=%-9llu p50=%-7llu p90=%-7llu p99=%-7llu "
                  "p99.9=%-7llu p99.99=%-7llu max=%llu (ns)",
                  label, static_cast<unsigned long long>(count_),
                  static_cast<unsigned long long>(percentile(50)),
                  static_cast<unsigned long long>(percentile(90)),
                  static_cast<unsigned long long>(percentile(99)),
                  static_cast<unsigned long long>(percentile(99.9)),
                  static_cast<unsigned long long>(percentile(99.99)),
                  static_cast<unsigned long long>(max_));
    return buf;
  }

 private:
  static constexpr int kMinorBits = 4;  // 16 minor buckets per octave

  static std::size_t slot(std::uint64_t v) {
    if (v < (1u << kMinorBits)) return static_cast<std::size_t>(v);
    const int msb = 63 - std::countl_zero(v);
    const auto major = static_cast<std::size_t>(msb - kMinorBits + 1);
    const auto minor =
        static_cast<std::size_t>((v >> (msb - kMinorBits)) & ((1u << kMinorBits) - 1));
    return (major << kMinorBits) + minor;
  }

  static std::uint64_t upper_bound(std::size_t s) {
    const std::size_t major = s >> kMinorBits;
    const std::size_t minor = s & ((1u << kMinorBits) - 1);
    if (major == 0) return minor;
    const int shift = static_cast<int>(major) - 1;
    return ((1ULL << kMinorBits) + minor + 1) << shift;
  }

  std::array<std::uint64_t, 1024> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = 0;
};

}  // namespace nsq

// Wall-clock timestamps as nanoseconds since local midnight, matching the
// ITCH 5.0 timestamp convention (48-bit ns-since-midnight fields).
#pragma once

#include <chrono>
#include <cstdint>

namespace nsq {

inline std::uint64_t ns_since_midnight() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto today = floor<days>(now);
  return static_cast<std::uint64_t>(duration_cast<nanoseconds>(now - today).count());
}

}  // namespace nsq

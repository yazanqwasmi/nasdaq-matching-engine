// Streams a raw-ITCH file into an ItchBookBuilder with optional symbol
// filtering and a message cap. Shared by the itchfile app and its tests.
#pragma once

#include "client/itch_book.hpp"
#include "itch/raw_itch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace nsq::client {

struct ItchFileStats {
  std::array<std::uint64_t, 256> decoded{};
  std::array<std::uint64_t, 256> skipped{};
  std::uint64_t filtered = 0;
  std::uint64_t total = 0;
  std::string error;
};

// Returns false on framing error (stats.error set). max_messages==0: no cap.
inline bool process_raw_itch(std::FILE* f, const std::vector<Symbol>& symbols,
                             std::uint64_t max_messages,
                             ItchBookBuilder& builder, ItchFileStats& stats) {
  const auto tracked = [&](const Symbol& s) {
    return symbols.empty() ||
           std::find(symbols.begin(), symbols.end(), s) != symbols.end();
  };

  itch::RawItchReader reader(f);
  while (max_messages == 0 || stats.total < max_messages) {
    const auto bytes = reader.next();
    if (!bytes) break;
    ++stats.total;
    const auto type = static_cast<unsigned char>((*bytes)[0]);
    const auto msg = itch::decode(bytes->data(), bytes->size());
    if (!msg) {
      ++stats.skipped[type];
      continue;
    }
    ++stats.decoded[type];
    // Drop adds for untracked symbols; later E/X/D/U messages for their
    // refs no-op inside the builder, keeping memory bounded.
    if (const auto* add = std::get_if<itch::AddOrder>(&*msg)) {
      if (!tracked(add->stock)) {
        ++stats.filtered;
        continue;
      }
    } else if (const auto* addm = std::get_if<itch::AddOrderMpid>(&*msg)) {
      if (!tracked(addm->add.stock)) {
        ++stats.filtered;
        continue;
      }
    }
    builder.on_message(*msg);
  }
  stats.error = reader.error();
  return stats.error.empty();
}

}  // namespace nsq::client

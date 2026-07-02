// Capture-file tools.
//   itchreplay dump <file>                 — print decoded packet contents
//   itchreplay replay <file> [--group G --port P] [--fast | --speed X]
//                                          — resend datagrams, paced by the
//                                            recorded timestamps (X-times
//                                            speed) or as fast as possible
#include "feed/capture.hpp"
#include "itch/itch.hpp"
#include "mold/mold.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

bool has_flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return true;
  return false;
}

const char* type_name(const nsq::itch::Message& m) {
  using namespace nsq::itch;
  return std::visit(
      [](const auto& v) -> const char* {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SystemEvent>) return "SystemEvent";
        else if constexpr (std::is_same_v<T, StockDirectory>) return "StockDirectory";
        else if constexpr (std::is_same_v<T, TradingAction>) return "TradingAction";
        else if constexpr (std::is_same_v<T, AddOrder>) return "AddOrder";
        else if constexpr (std::is_same_v<T, AddOrderMpid>) return "AddOrderMpid";
        else if constexpr (std::is_same_v<T, OrderExecuted>) return "OrderExecuted";
        else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) return "OrderExecutedWithPrice";
        else if constexpr (std::is_same_v<T, OrderCancel>) return "OrderCancel";
        else if constexpr (std::is_same_v<T, OrderDelete>) return "OrderDelete";
        else if constexpr (std::is_same_v<T, OrderReplace>) return "OrderReplace";
        else return "Trade";
      },
      m);
}

int dump(const std::string& path) {
  nsq::feed::CaptureReader r(path);
  if (!r.ok()) {
    std::fprintf(stderr, "itchreplay: cannot open %s\n", path.c_str());
    return 1;
  }
  std::uint64_t packets = 0, messages = 0;
  while (auto rec = r.next()) {
    const auto pkt = nsq::mold::parse(rec->bytes.data(), rec->bytes.size());
    if (!pkt) {
      std::printf("[%llu] UNPARSEABLE (%zu bytes)\n",
                  static_cast<unsigned long long>(rec->ts_ns),
                  rec->bytes.size());
      continue;
    }
    ++packets;
    if (pkt->end_of_session) {
      std::printf("[%llu] seq=%llu END-OF-SESSION\n",
                  static_cast<unsigned long long>(rec->ts_ns),
                  static_cast<unsigned long long>(pkt->seq));
      continue;
    }
    std::printf("[%llu] seq=%llu count=%zu:",
                static_cast<unsigned long long>(rec->ts_ns),
                static_cast<unsigned long long>(pkt->seq),
                pkt->messages.size());
    for (const auto& bytes : pkt->messages) {
      ++messages;
      if (const auto m = nsq::itch::decode(bytes.data(), bytes.size())) {
        std::printf(" %s", type_name(*m));
      } else {
        std::printf(" ?");
      }
    }
    std::printf("\n");
  }
  std::fprintf(stderr, "itchreplay: %llu packets, %llu messages\n",
               static_cast<unsigned long long>(packets),
               static_cast<unsigned long long>(messages));
  return 0;
}

int replay(const std::string& path, int argc, char** argv) {
  const char* group = arg_value(argc, argv, "--group", "239.192.0.1");
  const auto port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--port", "26000")));
  const bool fast = has_flag(argc, argv, "--fast");
  const double speed = std::atof(arg_value(argc, argv, "--speed", "1.0"));

  nsq::feed::CaptureReader r(path);
  if (!r.ok()) {
    std::fprintf(stderr, "itchreplay: cannot open %s\n", path.c_str());
    return 1;
  }

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  in_addr dest{};
  ::inet_pton(AF_INET, group, &dest);
  if ((ntohl(dest.s_addr) >> 24) >= 224 && (ntohl(dest.s_addr) >> 24) <= 239) {
    const std::uint8_t loop = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    in_addr ifaddr{};
    ::inet_pton(AF_INET, "127.0.0.1", &ifaddr);
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr);
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = dest;

  std::uint64_t sent = 0;
  std::uint64_t prev_ts = 0;
  while (auto rec = r.next()) {
    if (!fast && prev_ts != 0 && rec->ts_ns > prev_ts) {
      const auto gap_ns =
          static_cast<double>(rec->ts_ns - prev_ts) / (speed > 0 ? speed : 1);
      std::this_thread::sleep_for(
          std::chrono::nanoseconds(static_cast<std::uint64_t>(gap_ns)));
    }
    prev_ts = rec->ts_ns;
    ::sendto(fd, rec->bytes.data(), rec->bytes.size(), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ++sent;
  }
  ::close(fd);
  std::printf("itchreplay: replayed %llu packets to %s:%u%s\n",
              static_cast<unsigned long long>(sent), group, port,
              fast ? " (fast)" : "");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: itchreplay dump <file>\n"
                 "       itchreplay replay <file> [--group G --port P] "
                 "[--fast | --speed X]\n");
    return 1;
  }
  const std::string mode = argv[1];
  const std::string path = argv[2];
  if (mode == "dump") return dump(path);
  if (mode == "replay") return replay(path, argc, argv);
  std::fprintf(stderr, "itchreplay: unknown mode '%s'\n", mode.c_str());
  return 1;
}

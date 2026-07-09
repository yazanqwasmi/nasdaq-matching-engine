// The exchange daemon: OUCH/SoupBinTCP gateway + matching engine + ITCH/
// MoldUDP64 multicast feed, wired through queues. Ctrl-C shuts down in
// order (gateway, engine, feed) so the feed ends with an end-of-session.
#include "engine/engine.hpp"
#include "feed/feed.hpp"
#include "gateway/gateway.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

const char* arg_value(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

bool arg_flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return true;
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  const auto port =
      static_cast<std::uint16_t>(std::atoi(arg_value(argc, argv, "--port", "26400")));
  nsq::feed::FeedConfig fcfg;
  fcfg.dest_ip = arg_value(argc, argv, "--mcast", "239.192.0.1");
  fcfg.dest_port = static_cast<std::uint16_t>(
      std::atoi(arg_value(argc, argv, "--mcast-port", "26000")));
  fcfg.session = arg_value(argc, argv, "--session", "NSQSIM");
  fcfg.capture_path = arg_value(argc, argv, "--capture", "");

  const bool ll_mode = arg_flag(argc, argv, "--ll-mode");
  nsq::engine::CommandChannel to_engine{ll_mode};
  nsq::MpscQueue<nsq::engine::ClientResponse> to_gateway;
  nsq::MpscQueue<nsq::engine::MarketEvent> to_feed;

  nsq::engine::RunConfig ecfg;
  ecfg.lock_memory = ll_mode || arg_flag(argc, argv, "--lock-memory");
  ecfg.pin_core = std::atoi(arg_value(argc, argv, "--engine-core", "-1"));

  nsq::engine::Engine engine{to_engine, to_gateway, to_feed};
  nsq::gateway::Gateway gateway{port, to_engine, to_gateway};
  nsq::feed::FeedPublisher feed{to_feed, fcfg};

  engine.start(ecfg);
  feed.start();
  gateway.start();

  std::printf(
      "exchanged: OUCH gateway on tcp/%u, ITCH feed to %s:%u (%s)%s\n",
      gateway.port(), fcfg.dest_ip.c_str(), fcfg.dest_port,
      fcfg.session.c_str(), ll_mode ? "  [low-latency mode]" : "");
  std::fflush(stdout);

  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::puts("exchanged: shutting down");
  gateway.stop();
  engine.stop();
  feed.stop();  // flushes and sends end-of-session
  std::printf("exchanged: done (%llu feed packets)\n",
              static_cast<unsigned long long>(feed.packets_sent()));
  return 0;
}

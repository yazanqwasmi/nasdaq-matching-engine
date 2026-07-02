// OUCH order-entry gateway: a kqueue-based TCP server thread that owns all
// client sockets. Each connection runs a SoupBinTCP ServerSession; decoded
// OUCH commands pass basic risk checks and are queued to the engine, and
// engine responses are routed back by client id (the engine thread never
// touches sockets). The engine wakes the kqueue loop via a pipe write hooked
// to the response queue's notify callback.
//
// Simulator policy: all logins are accepted (credentials are not checked);
// documented as a non-goal.
#pragma once

#include "common/queue.hpp"
#include "engine/engine.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>

namespace nsq::gateway {

using ::nsq::MpscQueue;

// Per-order risk caps (documented subset of real pre-trade risk checks).
inline constexpr Qty kMaxOrderShares = 1'000'000;
inline constexpr Price kMaxOrderPrice = 1'999'999'900;  // $199,999.99

class Gateway {
 public:
  // port 0 binds an ephemeral port; see port().
  Gateway(std::uint16_t port, MpscQueue<engine::Command>& to_engine,
          MpscQueue<engine::ClientResponse>& from_engine);
  ~Gateway();

  void start();
  void stop();
  std::uint16_t port() const { return port_; }

 private:
  struct Conn;

  void run();
  void handle_accept();
  void handle_readable(Conn& conn);
  void handle_writable(Conn& conn);
  void handle_ouch(Conn& conn, const std::uint8_t* data, std::size_t n);
  void pump_responses();
  void tick_sessions();
  void flush(Conn& conn);
  void close_conn(Conn& conn);
  void update_write_filter(Conn& conn, bool enable);

  std::uint16_t port_;
  MpscQueue<engine::Command>& to_engine_;
  MpscQueue<engine::ClientResponse>& from_engine_;

  int listen_fd_ = -1;
  int kq_ = -1;
  int wake_pipe_[2] = {-1, -1};
  std::uint64_t next_client_id_ = 1;
  std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;
  std::unordered_map<int, Conn*> by_fd_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace nsq::gateway

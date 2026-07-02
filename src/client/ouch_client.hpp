// Blocking-connect, non-blocking-poll OUCH client over SoupBinTCP. Used by
// marketsim agents and tests; also sends periodic client heartbeats so the
// server's 15s receive timer never fires while idle.
#pragma once

#include "common/types.hpp"
#include "ouch/ouch.hpp"
#include "soup/soup.hpp"

#include <cstdint>
#include <string_view>

namespace nsq::client {

class OuchClient {
 public:
  struct Handler {
    virtual ~Handler() = default;
    virtual void on_accepted(const ouch::Accepted&) {}
    virtual void on_executed(const ouch::Executed&) {}
    virtual void on_canceled(const ouch::Canceled&) {}
    virtual void on_replaced(const ouch::Replaced&) {}
    virtual void on_rejected(const ouch::Rejected&) {}
    virtual void on_system_event(const ouch::SystemEvent&) {}
  };

  explicit OuchClient(Handler& handler) : handler_(handler) {}
  ~OuchClient() { close(); }

  // Blocks until login is accepted (or fails).
  bool connect(std::string_view host, std::uint16_t port,
               std::string_view username, std::string_view password);

  void enter(std::string_view token, Side side, std::string_view symbol,
             Price price, Qty shares);
  void cancel(std::string_view token, Qty keep_shares = 0);
  void replace(std::string_view existing, std::string_view replacement,
               Price price, Qty shares);

  // Drains everything currently readable, dispatching to the handler, and
  // sends a heartbeat if a second has passed since the last send.
  void poll();

  void close();
  bool connected() const { return fd_ >= 0; }

 private:
  void send_ouch(const std::uint8_t* msg, std::size_t n);
  void send_raw(const std::uint8_t* data, std::size_t n);
  void dispatch(const std::uint8_t* payload, std::size_t n);

  Handler& handler_;
  int fd_ = -1;
  soup::FrameParser parser_;
  std::uint64_t last_send_ns_ = 0;
};

}  // namespace nsq::client

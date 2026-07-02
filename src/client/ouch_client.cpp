#include "client/ouch_client.hpp"

#include "common/clock.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace nsq::client {

bool OuchClient::connect(std::string_view host, std::uint16_t port,
                         std::string_view username,
                         std::string_view password) {
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    close();
    return false;
  }
  const int one = 1;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  std::vector<std::uint8_t> out;
  soup::append_login_request(out, username, password, "", 1);
  send_raw(out.data(), out.size());

  // Block (2s cap) until login accepted/rejected.
  timeval tv{2, 0};
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  for (;;) {
    std::uint8_t buf[4096];
    const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) {
      close();
      return false;
    }
    parser_.push(buf, static_cast<std::size_t>(n));
    while (auto p = parser_.next()) {
      if (p->type == soup::kLoginAccepted) {
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        return true;
      }
      if (p->type == soup::kLoginRejected) {
        close();
        return false;
      }
    }
  }
}

void OuchClient::enter(std::string_view token, Side side,
                       std::string_view symbol, Price price, Qty shares) {
  ouch::EnterOrder m{};
  m.token = ouch::make_token(token);
  m.side = side;
  m.shares = shares;
  m.stock = make_symbol(symbol);
  m.price = price;
  m.tif = 99999;
  m.firm = {'S', 'I', 'M', ' '};
  m.display = 'Y';
  m.capacity = 'A';
  m.intermarket_sweep = 'N';
  m.cross_type = 'N';
  m.customer_type = 'R';
  std::uint8_t buf[ouch::kEnterOrderSize];
  ouch::encode(m, buf);
  send_ouch(buf, sizeof buf);
}

void OuchClient::cancel(std::string_view token, Qty keep_shares) {
  ouch::CancelOrder m{};
  m.token = ouch::make_token(token);
  m.shares = keep_shares;
  std::uint8_t buf[ouch::kCancelOrderSize];
  ouch::encode(m, buf);
  send_ouch(buf, sizeof buf);
}

void OuchClient::replace(std::string_view existing,
                         std::string_view replacement, Price price,
                         Qty shares) {
  ouch::ReplaceOrder m{};
  m.existing_token = ouch::make_token(existing);
  m.replacement_token = ouch::make_token(replacement);
  m.shares = shares;
  m.price = price;
  m.tif = 99999;
  m.display = 'Y';
  m.intermarket_sweep = 'N';
  std::uint8_t buf[ouch::kReplaceOrderSize];
  ouch::encode(m, buf);
  send_ouch(buf, sizeof buf);
}

void OuchClient::poll() {
  if (fd_ < 0) return;
  std::uint8_t buf[8192];
  for (;;) {
    const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n <= 0) break;
    parser_.push(buf, static_cast<std::size_t>(n));
  }
  while (auto p = parser_.next()) {
    if (p->type == soup::kSequenced && !p->payload.empty())
      dispatch(p->payload.data(), p->payload.size());
  }
  const std::uint64_t now = ns_since_midnight();
  if (now - last_send_ns_ > 1'000'000'000ULL) {
    std::vector<std::uint8_t> hb;
    soup::append_packet(hb, soup::kClientHeartbeat, nullptr, 0);
    send_raw(hb.data(), hb.size());
  }
}

void OuchClient::dispatch(const std::uint8_t* payload, std::size_t n) {
  switch (static_cast<char>(payload[0])) {
    case 'A':
      if (const auto m = ouch::decode_accepted(payload, n))
        handler_.on_accepted(*m);
      break;
    case 'E':
      if (const auto m = ouch::decode_executed(payload, n))
        handler_.on_executed(*m);
      break;
    case 'C':
      if (const auto m = ouch::decode_canceled(payload, n))
        handler_.on_canceled(*m);
      break;
    case 'U':
      if (const auto m = ouch::decode_replaced(payload, n))
        handler_.on_replaced(*m);
      break;
    case 'J':
      if (const auto m = ouch::decode_rejected(payload, n))
        handler_.on_rejected(*m);
      break;
    case 'S':
      if (const auto m = ouch::decode_system_event(payload, n))
        handler_.on_system_event(*m);
      break;
    default:
      break;
  }
}

void OuchClient::send_ouch(const std::uint8_t* msg, std::size_t n) {
  std::vector<std::uint8_t> out;
  soup::append_packet(out, soup::kUnsequenced, msg, n);
  send_raw(out.data(), out.size());
}

void OuchClient::send_raw(const std::uint8_t* data, std::size_t n) {
  if (fd_ < 0) return;
  (void)!::send(fd_, data, n, 0);
  last_send_ns_ = ns_since_midnight();
}

void OuchClient::close() {
  if (fd_ >= 0) {
    std::vector<std::uint8_t> out;
    soup::append_packet(out, soup::kLogoutRequest, nullptr, 0);
    (void)!::send(fd_, out.data(), out.size(), 0);
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace nsq::client

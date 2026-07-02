#include "gateway/gateway.hpp"

#include "common/clock.hpp"
#include "ouch/ouch.hpp"
#include "soup/soup.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nsq::gateway {

namespace {

constexpr uintptr_t kTimerIdent = 1;
constexpr int kTickMs = 500;

void set_nonblocking(int fd) {
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

constexpr std::size_t encoded_size(const ouch::SystemEvent&) {
  return ouch::kSystemEventSize;
}
constexpr std::size_t encoded_size(const ouch::Accepted&) {
  return ouch::kAcceptedSize;
}
constexpr std::size_t encoded_size(const ouch::Executed&) {
  return ouch::kExecutedSize;
}
constexpr std::size_t encoded_size(const ouch::Canceled&) {
  return ouch::kCanceledSize;
}
constexpr std::size_t encoded_size(const ouch::Replaced&) {
  return ouch::kReplacedSize;
}
constexpr std::size_t encoded_size(const ouch::Rejected&) {
  return ouch::kRejectedSize;
}

template <typename Variant>
std::vector<std::uint8_t> encode_ouch(const Variant& msg) {
  return std::visit(
      [](const auto& m) {
        std::vector<std::uint8_t> buf(encoded_size(m));
        ouch::encode(m, buf.data());
        return buf;
      },
      msg);
}

}  // namespace

struct Gateway::Conn : soup::ServerSession::Listener {
  Gateway& gw;
  int fd;
  std::uint64_t id;
  soup::ServerSession session;
  std::vector<std::uint8_t> outbuf;
  bool write_enabled = false;

  Conn(Gateway& g, int f, std::uint64_t i, std::uint64_t now)
      : gw(g), fd(f), id(i), session(*this, now) {}

  void on_login_request(const soup::LoginRequest&) override {
    // Simulator policy: all credentials accepted.
    session.accept_login("SIM01", 1);
  }

  void on_unsequenced(const std::uint8_t* data, std::size_t n) override {
    gw.handle_ouch(*this, data, n);
  }
};

Gateway::Gateway(std::uint16_t port, MpscQueue<engine::Command>& to_engine,
                 MpscQueue<engine::ClientResponse>& from_engine)
    : port_(port), to_engine_(to_engine), from_engine_(from_engine) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
  if (::listen(listen_fd_, 64) != 0)
    throw std::runtime_error("listen() failed");
  set_nonblocking(listen_fd_);

  socklen_t len = sizeof addr;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);

  if (::pipe(wake_pipe_) != 0) throw std::runtime_error("pipe() failed");
  set_nonblocking(wake_pipe_[0]);
  set_nonblocking(wake_pipe_[1]);
  from_engine_.set_notify([fd = wake_pipe_[1]] {
    const char b = 1;
    (void)!::write(fd, &b, 1);
  });
}

Gateway::~Gateway() {
  stop();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  ::close(wake_pipe_[0]);
  ::close(wake_pipe_[1]);
}

void Gateway::start() {
  thread_ = std::thread([this] { run(); });
}

void Gateway::stop() {
  if (!thread_.joinable()) return;
  stopping_ = true;
  const char b = 1;
  (void)!::write(wake_pipe_[1], &b, 1);
  thread_.join();
}

void Gateway::run() {
  kq_ = ::kqueue();
  struct kevent evs[3];
  EV_SET(&evs[0], static_cast<uintptr_t>(listen_fd_), EVFILT_READ, EV_ADD, 0,
         0, nullptr);
  EV_SET(&evs[1], static_cast<uintptr_t>(wake_pipe_[0]), EVFILT_READ, EV_ADD,
         0, 0, nullptr);
  EV_SET(&evs[2], kTimerIdent, EVFILT_TIMER, EV_ADD, 0, kTickMs, nullptr);
  ::kevent(kq_, evs, 3, nullptr, 0, nullptr);

  struct kevent events[64];
  while (!stopping_) {
    const int n = ::kevent(kq_, nullptr, 0, events, 64, nullptr);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n && !stopping_; ++i) {
      const auto& ev = events[i];
      if (ev.filter == EVFILT_TIMER) {
        tick_sessions();
      } else if (ev.ident == static_cast<uintptr_t>(listen_fd_)) {
        handle_accept();
      } else if (ev.ident == static_cast<uintptr_t>(wake_pipe_[0])) {
        char buf[256];
        while (::read(wake_pipe_[0], buf, sizeof buf) > 0) {
        }
        pump_responses();
      } else {
        const auto it = by_fd_.find(static_cast<int>(ev.ident));
        if (it == by_fd_.end()) continue;
        if (ev.filter == EVFILT_READ) {
          handle_readable(*it->second);
        } else if (ev.filter == EVFILT_WRITE) {
          handle_writable(*it->second);
        }
      }
    }
  }

  for (auto& [id, conn] : conns_) ::close(conn->fd);
  conns_.clear();
  by_fd_.clear();
  ::close(kq_);
  kq_ = -1;
}

void Gateway::handle_accept() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    set_nonblocking(fd);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    const std::uint64_t id = next_client_id_++;
    auto conn = std::make_unique<Conn>(*this, fd, id, ns_since_midnight());
    by_fd_[fd] = conn.get();
    struct kevent ev;
    EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD, 0, 0,
           nullptr);
    ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    conns_.emplace(id, std::move(conn));
  }
}

void Gateway::handle_readable(Conn& conn) {
  std::uint8_t buf[8192];
  for (;;) {
    const ssize_t n = ::read(conn.fd, buf, sizeof buf);
    if (n > 0) {
      conn.session.on_bytes(buf, static_cast<std::size_t>(n),
                            ns_since_midnight());
    } else if (n == 0) {
      close_conn(conn);
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      close_conn(conn);
      return;
    }
  }
  flush(conn);
  if (conn.session.state() == soup::ServerSession::State::Closed &&
      conn.outbuf.empty()) {
    close_conn(conn);
  }
}

void Gateway::handle_ouch(Conn& conn, const std::uint8_t* data,
                          std::size_t n) {
  if (n == 0) return;
  const auto reject = [&](const ouch::Token& token) {
    ouch::Rejected r{};
    r.timestamp = ns_since_midnight();
    r.token = token;
    r.reason = 'R';  // risk
    std::uint8_t buf[ouch::kRejectedSize];
    ouch::encode(r, buf);
    conn.session.send_sequenced(buf, sizeof buf);
  };

  switch (static_cast<char>(data[0])) {
    case 'O': {
      const auto m = ouch::decode_enter_order(data, n);
      if (!m) return;
      if (m->shares > kMaxOrderShares || m->price > kMaxOrderPrice)
        return reject(m->token);
      to_engine_.push({conn.id, *m});
      break;
    }
    case 'X': {
      const auto m = ouch::decode_cancel_order(data, n);
      if (m) to_engine_.push({conn.id, *m});
      break;
    }
    case 'U': {
      const auto m = ouch::decode_replace_order(data, n);
      if (!m) return;
      if (m->shares > kMaxOrderShares || m->price > kMaxOrderPrice)
        return reject(m->replacement_token);
      to_engine_.push({conn.id, *m});
      break;
    }
    default:
      break;
  }
}

void Gateway::pump_responses() {
  for (auto& resp : from_engine_.drain()) {
    const auto it = conns_.find(resp.client_id);
    if (it == conns_.end()) continue;  // client disconnected meanwhile
    const auto bytes = encode_ouch(resp.msg);
    it->second->session.send_sequenced(bytes.data(), bytes.size());
    flush(*it->second);
  }
}

void Gateway::tick_sessions() {
  const std::uint64_t now = ns_since_midnight();
  std::vector<std::uint64_t> to_close;
  for (auto& [id, conn] : conns_) {
    conn->session.tick(now);
    flush(*conn);
    if (conn->session.state() == soup::ServerSession::State::Closed &&
        conn->outbuf.empty()) {
      to_close.push_back(id);
    }
  }
  for (const auto id : to_close) {
    const auto it = conns_.find(id);
    if (it != conns_.end()) close_conn(*it->second);
  }
}

void Gateway::flush(Conn& conn) {
  const auto fresh = conn.session.take_output();
  conn.outbuf.insert(conn.outbuf.end(), fresh.begin(), fresh.end());
  if (conn.outbuf.empty()) return;

  const ssize_t n = ::send(conn.fd, conn.outbuf.data(), conn.outbuf.size(), 0);
  if (n > 0) {
    conn.outbuf.erase(conn.outbuf.begin(), conn.outbuf.begin() + n);
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    close_conn(conn);
    return;
  }
  update_write_filter(conn, !conn.outbuf.empty());
}

void Gateway::handle_writable(Conn& conn) { flush(conn); }

void Gateway::update_write_filter(Conn& conn, bool enable) {
  if (enable == conn.write_enabled) return;
  struct kevent ev;
  EV_SET(&ev, static_cast<uintptr_t>(conn.fd), EVFILT_WRITE,
         enable ? EV_ADD : EV_DELETE, 0, 0, nullptr);
  ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
  conn.write_enabled = enable;
}

void Gateway::close_conn(Conn& conn) {
  ::close(conn.fd);  // closing the fd removes its kqueue filters
  by_fd_.erase(conn.fd);
  conns_.erase(conn.id);  // destroys conn — do not touch it after this
}

}  // namespace nsq::gateway

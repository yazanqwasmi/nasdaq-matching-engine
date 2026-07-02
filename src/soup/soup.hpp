// SoupBinTCP framing and server-side session state machine.
//
// Packet: uint16 big-endian length (type byte + payload), 1-byte type,
// payload. The ServerSession is a pure state machine — bytes in, bytes out,
// explicit nanosecond time — so it is testable without sockets; the gateway
// owns the actual TCP connection and pumps this object.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nsq::soup {

inline constexpr char kLoginRequest = 'L';
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequenced = 'S';
inline constexpr char kUnsequenced = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kDebug = '+';
inline constexpr char kLogoutRequest = 'O';
inline constexpr char kEndOfSession = 'Z';

// Server sends a heartbeat after 1s of send-silence; either side may drop
// the session after 15s of receive-silence (values per the Soup spec).
inline constexpr std::uint64_t kHeartbeatIntervalNs = 1'000'000'000ULL;
inline constexpr std::uint64_t kSessionTimeoutNs = 15'000'000'000ULL;

void append_packet(std::vector<std::uint8_t>& out, char type,
                   const std::uint8_t* payload, std::size_t n);

void append_login_request(std::vector<std::uint8_t>& out,
                          std::string_view username, std::string_view password,
                          std::string_view requested_session,
                          std::uint64_t requested_seq);

struct LoginRequest {
  std::string username;
  std::string password;
  std::string requested_session;
  std::uint64_t requested_seq;
};

// `p`/`n` are the packet payload (type byte excluded).
std::optional<LoginRequest> decode_login_request(const std::uint8_t* p,
                                                 std::size_t n);

class FrameParser {
 public:
  struct Packet {
    char type;
    std::vector<std::uint8_t> payload;
  };

  void push(const std::uint8_t* data, std::size_t n);
  std::optional<Packet> next();

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t consumed_ = 0;
};

class ServerSession {
 public:
  struct Listener {
    virtual ~Listener() = default;
    virtual void on_login_request(const LoginRequest&) {}
    virtual void on_unsequenced(const std::uint8_t*, std::size_t) {}
    virtual void on_logout() {}
    virtual void on_timeout() {}
  };

  enum class State { AwaitingLogin, LoggedIn, Closed };

  ServerSession(Listener& listener, std::uint64_t now_ns)
      : listener_(listener), last_recv_ns_(now_ns), last_send_ns_(now_ns) {}

  void on_bytes(const std::uint8_t* data, std::size_t n, std::uint64_t now_ns);
  void tick(std::uint64_t now_ns);

  void accept_login(std::string_view session, std::uint64_t next_seq);
  void reject_login(char reason);
  void send_sequenced(const std::uint8_t* payload, std::size_t n);
  void send_end_of_session();

  State state() const { return state_; }
  std::vector<std::uint8_t> take_output();

 private:
  void queue(char type, const std::uint8_t* payload, std::size_t n);

  Listener& listener_;
  FrameParser parser_;
  State state_ = State::AwaitingLogin;
  std::vector<std::uint8_t> out_;
  std::uint64_t last_recv_ns_;
  std::uint64_t last_send_ns_;
};

}  // namespace nsq::soup

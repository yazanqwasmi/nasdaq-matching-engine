// Cross-platform readiness poller for the gateway event loop. Wraps kqueue
// (macOS/BSD) or epoll (Linux) behind one small level-triggered interface:
// register fds for read, toggle write interest, and wait with a timeout.
//
// This is the only event-loop code that differs per OS; the gateway is
// written against this interface so the kqueue<->epoll choice is a
// compile-time detail confined to poller.cpp.
#pragma once

#include <vector>

namespace nsq::gateway {

class Poller {
 public:
  struct Event {
    int fd;
    bool readable;
    bool writable;
  };

  Poller();
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  // Register fd for read-readiness notifications (write starts disabled).
  void add_read(int fd);
  // Enable/disable write-readiness notifications for an already-added fd.
  void set_write(int fd, bool enable);
  // Stop watching fd. Must be called before ::close(fd) so the fd number is
  // safe to reuse. Best-effort: unknown fds are ignored.
  void remove(int fd);

  // Block up to timeout_ms for readiness, replacing the contents of `out`
  // with the ready events. Returns the number of events (0 on timeout or a
  // signal), or -1 on a hard error.
  int wait(std::vector<Event>& out, int timeout_ms);

 private:
  int poll_fd_ = -1;
};

}  // namespace nsq::gateway

#include "gateway/poller.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <ctime>
#endif

namespace nsq::gateway {

#if defined(__linux__)

Poller::Poller() {
  poll_fd_ = ::epoll_create1(0);
  if (poll_fd_ < 0) throw std::runtime_error("epoll_create1() failed");
}

Poller::~Poller() {
  if (poll_fd_ >= 0) ::close(poll_fd_);
}

void Poller::add_read(int fd) {
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void Poller::set_write(int fd, bool enable) {
  epoll_event ev{};
  ev.events = static_cast<std::uint32_t>(EPOLLIN | (enable ? EPOLLOUT : 0));
  ev.data.fd = fd;
  ::epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void Poller::remove(int fd) {
  ::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Poller::wait(std::vector<Event>& out, int timeout_ms) {
  out.clear();
  epoll_event evs[64];
  const int n = ::epoll_wait(poll_fd_, evs, 64, timeout_ms);
  if (n < 0) return errno == EINTR ? 0 : -1;
  for (int i = 0; i < n; ++i) {
    const std::uint32_t e = evs[i].events;
    out.push_back({evs[i].data.fd,
                   (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0,
                   (e & EPOLLOUT) != 0});
  }
  return n;
}

#else  // kqueue (macOS / BSD)

Poller::Poller() {
  poll_fd_ = ::kqueue();
  if (poll_fd_ < 0) throw std::runtime_error("kqueue() failed");
}

Poller::~Poller() {
  if (poll_fd_ >= 0) ::close(poll_fd_);
}

void Poller::add_read(int fd) {
  struct kevent ev;
  EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD, 0, 0, nullptr);
  ::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
}

void Poller::set_write(int fd, bool enable) {
  struct kevent ev;
  EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE,
         enable ? EV_ADD : EV_DELETE, 0, 0, nullptr);
  ::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
}

void Poller::remove(int fd) {
  // Delete each filter independently so a missing one doesn't mask the other.
  struct kevent ev;
  EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  ::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
  EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0,
         nullptr);
  ::kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
}

int Poller::wait(std::vector<Event>& out, int timeout_ms) {
  out.clear();
  struct kevent evs[64];
  struct timespec ts;
  ts.tv_sec = timeout_ms / 1000;
  ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
  const int n = ::kevent(poll_fd_, nullptr, 0, evs, 64, &ts);
  if (n < 0) return errno == EINTR ? 0 : -1;
  for (int i = 0; i < n; ++i) {
    out.push_back({static_cast<int>(evs[i].ident),
                   evs[i].filter == EVFILT_READ,
                   evs[i].filter == EVFILT_WRITE});
  }
  return n;
}

#endif

}  // namespace nsq::gateway

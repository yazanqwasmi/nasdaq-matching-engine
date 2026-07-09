// Hot-path CPU/memory hygiene for the low-latency engine mode: a spin-wait
// relax hint, thread-to-core pinning, and memory locking to keep the matching
// thread off the scheduler and out of the page-fault path.
//
// Core pinning is real on Linux (pthread_setaffinity_np) and a best-effort
// no-op on macOS, which exposes no equivalent hard-affinity API — the busy
// poll and mlock still apply there. Each function reports whether it took
// effect so callers can log honestly.
#pragma once

#include <sched.h>

#include <cstdio>

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#elif defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace nsq {

// Pause/yield hint inside a busy-wait loop: lowers power and frees the
// pipeline/SMT sibling without yielding the core to the scheduler.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

// Pin the calling thread to `core`. Returns true only if the OS applied a
// hard affinity (Linux). macOS has no equivalent, so this returns false.
inline bool pin_thread_to_core(int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
  (void)core;
  return false;
#endif
}

// Lock the process's pages resident (current + future) so the hot path never
// takes a page fault. Requires privilege/limits on some systems; returns
// false if the kernel refused.
inline bool lock_all_memory() {
#if defined(MCL_CURRENT) && defined(MCL_FUTURE)
  return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
  return false;
#endif
}

}  // namespace nsq

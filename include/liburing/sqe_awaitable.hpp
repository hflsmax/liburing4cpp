#pragma once

#include <climits>
#include <liburing.h>
#include <type_traits>
#include <optional>
#include <cassert>
#include <shared_mutex>
#include <liburing/stdlib_coroutine.hpp>

struct spinlock {
  std::atomic<bool> lock_ = {0};

  void lock() noexcept {
    for (;;) {
      // Optimistically assume the lock is free on the first try
      if (!lock_.exchange(true, std::memory_order_acquire)) {
        return;
      }
      // Wait for lock to be released without generating cache misses
      while (lock_.load(std::memory_order_relaxed)) {}
    }
  }

  bool try_lock() noexcept {
    // First do a relaxed load to check if lock is free in order to prevent
    // unnecessary cache misses if someone does while(!try_lock())
    return !lock_.load(std::memory_order_relaxed) &&
           !lock_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept {
    lock_.store(false, std::memory_order_release);
  }
};

struct spinlock spin;
std::mutex sq_mutex;


namespace uio {
struct resume_resolver {
    friend struct sqe_awaitable;

    std::coroutine_handle<> resolve(int result) noexcept {
        this->result = result;
        return handle;
    }

private:
    std::coroutine_handle<> handle;
    int result = 0;
};
static_assert(std::is_trivially_destructible_v<resume_resolver>);

struct sqe_awaitable {
    // TODO: use cancel_token to implement cancellation
    sqe_awaitable(io_uring_sqe* sqe) noexcept: sqe(sqe) {}

    resume_resolver resolver {};

    constexpr bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        resolver.handle = handle;
        io_uring_sqe_set_data(sqe, &resolver);
        sq_mutex.unlock();
    }

    constexpr int await_resume() const noexcept { return resolver.result; }

private:
    io_uring_sqe* sqe;
};

} // namespace uio

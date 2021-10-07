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
struct resolver {
    virtual void resolve(int result) noexcept = 0;
};

struct resume_resolver final: resolver {
    friend struct sqe_awaitable;

    void resolve(int result) noexcept override {
        this->result = result;
    }

    void resume() {
      fmt::print("resume on {}\n", handle.address());
      handle.resume();
    }

private:
    std::coroutine_handle<> handle;
    int result = 0;
};
static_assert(std::is_trivially_destructible_v<resume_resolver>);

struct deferred_resolver final: resolver {
    void resolve(int result) noexcept override {
        this->result = result;
    }

#ifndef NDEBUG
    ~deferred_resolver() {
        assert(!!result && "deferred_resolver is destructed before it's resolved");
    }
#endif

    std::optional<int> result;
};

struct callback_resolver final: resolver {
    callback_resolver(std::function<void (int result)>&& cb): cb(std::move(cb)) {}

    void resolve(int result) noexcept override {
        this->cb(result);
        delete this;
    }

private:
    std::function<void (int result)> cb;
};

struct sqe_awaitable {
    // TODO: use cancel_token to implement cancellation
    sqe_awaitable(io_uring_sqe* sqe) noexcept: sqe(sqe) {}

    // User MUST keep resolver alive before the operation is finished
    void set_deferred(deferred_resolver& resolver) {
        io_uring_sqe_set_data(sqe, &resolver);
    }

    void set_callback(std::function<void (int result)> cb) {
        io_uring_sqe_set_data(sqe, new callback_resolver(std::move(cb)));
    }

    auto operator co_await() {
        struct await_sqe {
            resume_resolver resolver {};
            io_uring_sqe* sqe;

            await_sqe(io_uring_sqe* sqe): sqe(sqe) {}

            constexpr bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                fmt::print("provide handle, {}\n", handle.address());
                resolver.handle = handle;
                io_uring_sqe_set_data(sqe, &resolver);
                sq_mutex.unlock();
                // fmt::print("await_suspend\n");
            }

            constexpr int await_resume() const noexcept { return resolver.result; }
        };

        return await_sqe(sqe);
    }

private:
    io_uring_sqe* sqe;
};

} // namespace uio

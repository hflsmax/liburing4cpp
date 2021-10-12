#pragma once

#include <exception>
#include <variant>
#include <array>
#include <cassert>
#include <thread>

#include <liburing/stdlib_coroutine.hpp>

namespace uio {
template <typename T, bool nothrow, bool entry_task, bool detached>
struct task;

// only for internal usage
template <typename T, bool nothrow, bool entry_task, bool detached>
struct task_promise_base {
    task<T, nothrow, entry_task, detached> get_return_object();
    auto initial_suspend() { return std::suspend_never(); }
    auto final_suspend() noexcept {
        while (caller_state == NOT_READY) {}
        struct Awaiter: std::suspend_always {
            std::coroutine_handle<> continuation;
            CallerState caller_state;

            Awaiter(std::coroutine_handle<> continuation, CallerState caller_state): continuation(continuation), caller_state(caller_state) {};
            std::coroutine_handle<> await_suspend(__attribute__((unused)) std::coroutine_handle<> coro) const noexcept {
                if (caller_state == READY_TO_RESUME) {
                    return continuation;
                }
                if (caller_state == NO_CONTINUE) {
                    // If we are not going to continue to the caller who will destroy us
                    // we self destroys
                    coro.destroy();
                }
                return std::noop_coroutine();
            }
        };
        return Awaiter(waiter_, caller_state);
    }
    void unhandled_exception() {
        if constexpr (!nothrow) {
            result_.template emplace<2>(std::current_exception());
        } else {
            __builtin_unreachable();
        }
    }

    // NOT_READY: the caller has not provide the waiter_ or instruct not to resume yet
    // READY_TO_RESUME: callee continue to caller, and caller destroy callee
    // NO_CONTINUE: callee does not continue to caller, and callee destroy itself
    // ENTRY_TASK: callee does not continue to caller, and caller destroy callee
    enum CallerState {NOT_READY, READY_TO_RESUME, NO_CONTINUE, ENTRY_TASK};
    CallerState caller_state {detached ? NO_CONTINUE : (entry_task ? ENTRY_TASK : NOT_READY)};

protected:
    friend struct task<T, nothrow, entry_task, detached>;
    task_promise_base() = default;
    std::coroutine_handle<> waiter_;
    std::variant<
        std::monostate,
        std::conditional_t<std::is_void_v<T>, std::monostate, T>,
        std::conditional_t<!nothrow, std::exception_ptr, std::monostate>
    > result_;
};

// only for internal usage
template <typename T, bool nothrow, bool entry_task, bool detached>
struct task_promise final: task_promise_base<T, nothrow, entry_task, detached> {
    using task_promise_base<T, nothrow, entry_task, detached>::result_;

    template <typename U>
    void return_value(U&& u) {
        result_.template emplace<1>(static_cast<U&&>(u));
    }
    void return_value(int u) {
        result_.template emplace<1>(u);
    }
};

template <bool nothrow, bool entry_task, bool detached>
struct task_promise<void, nothrow, entry_task, detached> final: task_promise_base<void, nothrow, entry_task, detached> {
    using task_promise_base<void, nothrow, entry_task, detached>::result_;

    void return_void() {
        result_.template emplace<1>(std::monostate {});
    }
};

/**
 * An awaitable object that returned by an async function
 * @tparam T value type holded by this task
 * @tparam nothrow if true, the coroutine assigned by this task won't throw exceptions ( slightly better performance )
 * @tparam entry_task if true, the coroutine does not continue to another coroutine after final_suspend
 * @tparam detached if true, the coroutine does not continue to another coroutine after final_suspend and needs to destroy itself
 * @warning do NOT discard this object when returned by some function, or UB WILL happen
 */
template <typename T = void, bool nothrow = false, bool entry_task = false, bool detached = false>
struct task final {
    using promise_type = task_promise<T, nothrow, entry_task, detached>;
    using handle_t = std::coroutine_handle<promise_type>;

    task(const task&) = delete;
    task& operator =(const task&) = delete;

    bool await_ready() {
        auto& result_ = coro_.promise().result_;
        if (result_.index() > 0) {
            static_cast<promise_type&>(coro_.promise()).caller_state = promise_type::NO_CONTINUE;
            // if the result is immediately ready, we don't wait to destroy callee
            // let callee destroy itself
            destroy_callee = false;
            return true;
        } else {
            return false;
        }
    }

    template <typename T_, bool nothrow_, bool entry_task_, bool detached_>
    void await_suspend(std::coroutine_handle<task_promise<T_, nothrow_, entry_task_, detached_>> caller) noexcept {
        coro_.promise().waiter_ = caller;
        static_cast<promise_type&>(coro_.promise()).caller_state = promise_type::READY_TO_RESUME;
    }

    T await_resume() const {
        return get_result();
    }

    /** Get the result hold by this task */
    T get_result() const {
        auto& result_ = coro_.promise().result_;
        if constexpr (!nothrow) {
            if (auto* pep = std::get_if<2>(&result_)) {
                std::rethrow_exception(*pep);
            }
        }
        if constexpr (!std::is_void_v<T>) {
            return *std::get_if<1>(&result_);
        }
    }

    bool done() const {
        return coro_.done();
    }

    /** Only for placeholder */
    task(): coro_(nullptr) {};

    task(task&& other) noexcept {
        coro_ = std::exchange(other.coro_, nullptr);
    }

    task& operator =(task&& other) noexcept {
        if (coro_) coro_.destroy();
        coro_ = std::exchange(other.coro_, nullptr);
        return *this;
    }

    /** Destroy (when done) or detach (when not done) the task object */
    ~task() {
        if (destroy_callee) {
            coro_.destroy();
        }
    }

private:
    friend struct task_promise_base<T, nothrow, entry_task, detached>;
    task(promise_type *p): coro_(handle_t::from_promise(*p)) {}
    handle_t coro_;

    // if the callee is not detached, by default the task is responsible to destroy it
    bool destroy_callee = !detached;
};

template <typename T, bool nothrow, bool entry_task, bool detached>
task<T, nothrow, entry_task, detached> task_promise_base<T, nothrow, entry_task, detached>::get_return_object() {
    return task<T, nothrow, entry_task, detached>(static_cast<task_promise<T, nothrow, entry_task, detached> *>(this));
}

} // namespace uio

#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace tbcad::cmd {

// Coroutine return type of Command::run and of helper coroutines. Tasks are lazy: nothing runs
// until the owner resumes them (the CommandRunner does that) or another Task awaits them.
//
//   Task helper(CommandContext& ctx) { ...; co_return; }
//   Task Foo::run(CommandContext& ctx) { co_await helper(ctx); }
//
// Awaiting a Task uses symmetric transfer, so deep chains do not grow the stack.
class Task {
public:
    struct promise_type {
        std::coroutine_handle<> continuation{};
        std::exception_ptr exception{};

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                auto next = h.promise().continuation;
                return next ? next : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(Handle h) : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    explicit operator bool() const { return static_cast<bool>(h_); }
    bool done() const { return !h_ || h_.done(); }

    // Runs the task until its first suspension point that hands control back to the caller
    // (a prompt) or until it finishes. Does not rethrow; see rethrowIfFailed().
    void resume() {
        if (h_ && !h_.done())
            h_.resume();
    }
    void rethrowIfFailed() const {
        if (h_ && h_.promise().exception)
            std::rethrow_exception(h_.promise().exception);
    }

    struct Awaiter {
        Handle h;
        bool await_ready() const noexcept { return !h || h.done(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
            h.promise().continuation = caller;
            return h;
        }
        void await_resume() const {
            if (h && h.promise().exception)
                std::rethrow_exception(h.promise().exception);
        }
    };
    // Awaiting a sub-task: `co_await helper(ctx);`
    Awaiter operator co_await() && noexcept { return Awaiter{h_}; }

private:
    void reset() {
        if (h_) {
            h_.destroy();
            h_ = {};
        }
    }
    Handle h_{};
};

}  // namespace tbcad::cmd

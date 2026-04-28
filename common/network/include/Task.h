#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace network {

template <typename T = void>
class [[nodiscard]] Task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type handle) noexcept : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    void await_suspend(std::coroutine_handle<> continuation);
    T await_resume();

private:
    handle_type handle_;
};

template <typename T>
struct Task<T>::promise_type {
    std::coroutine_handle<> continuation_{};
    std::optional<T> value_;
    std::exception_ptr exception_{};

    Task get_return_object() noexcept {
        return Task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
            auto continuation = handle.promise().continuation_;
            return continuation ? continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() const noexcept { return {}; }

    template <typename U>
        requires std::convertible_to<U, T>
    void return_value(U&& value) noexcept(std::is_nothrow_convertible_v<U, T>) {
        value_.emplace(std::forward<U>(value));
    }

    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
};

template <typename T>
void Task<T>::await_suspend(std::coroutine_handle<> continuation) {
    handle_.promise().continuation_ = continuation;
    handle_.resume();
}

template <typename T>
T Task<T>::await_resume() {
    auto& promise = handle_.promise();
    if (promise.exception_) {
        std::rethrow_exception(promise.exception_);
    }
    return std::move(*(promise.value_));
}

template <>
class [[nodiscard]] Task<void> {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type handle) noexcept : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    void await_suspend(std::coroutine_handle<> continuation);
    void await_resume();

private:
    handle_type handle_;
};

struct Task<void>::promise_type {
    std::coroutine_handle<> continuation_{};
    std::exception_ptr exception_{};

    Task get_return_object() noexcept {
        return Task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
            auto continuation = handle.promise().continuation_;
            return continuation ? continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() const noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
};

inline void Task<void>::await_suspend(std::coroutine_handle<> continuation) {
    handle_.promise().continuation_ = continuation;
    handle_.resume();
}

inline void Task<void>::await_resume() {
    auto& promise = handle_.promise();
    if (promise.exception_) {
        std::rethrow_exception(promise.exception_);
    }
}

}  // namespace network

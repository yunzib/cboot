export module cboot.async;

import std;

/**
 * @brief 协程基建 — 自写 task<T> + 事件桥（工具链无 <task>/std::execution）
 *
 * 定位：
 *   - C++26 标准有 std::task（P3552）与 std::execution（P2300），但 GCC 16.2
 *     均未提供（已实测）——本模块是最小可用替代
 *   - 惰性启动（initial_suspend=always）：co_await / start() 时才运行
 *   - 对称转移：内层 task 完成后直接恢复外层（无额外调度开销）
 *   - void/非 void 由 promise 特化区分（CRTP 基类 + final_awaiter 模板）
 *
 * 语义：
 *   - `co_await inner_task`：要求 inner 为右值（临时）或 std::move
 *   - 异常经 promise 捕获，由 await_resume / sync_wait 重新抛出
 *   - 外部事件等待用 awaitable_event（由 I/O 后端完成通知时 set()）
 *
 * 当前边界：无自带调度器。sync_wait 只适用于"不会挂起在外部事件上"的任务；
 * 事件驱动场景由宿主循环（IOCP/epoll）在完成时 set() 并恢复协程。
 *
 * 生命周期陷阱（Release 才暴露的 UB，实测踩过）：
 *   协程 lambda（`[&]{ ... co_await ... }()`）的捕获位于闭包对象中，而闭包
 *   临时量在 full-expression 末尾即销毁——协程帧此后引用其捕获即悬空。
 *   规范：协程一律用具名函数/成员函数（捕获经参数传递），或保证闭包对象
 *   的寿命覆盖任务全程。
 */

export template <class T = void>
class task;

namespace async_detail {

/** @brief promise 公共部分（CRTP：final_awaiter 需知道具体 promise 类型） */
template <class Derived>
struct promise_base {
    std::exception_ptr error;
    std::coroutine_handle<> continuation{};

    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Derived> h) noexcept {
            auto cont = h.promise().continuation;
            return cont ? cont : std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() noexcept { return {}; }
    final_awaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class T>
struct promise_for : promise_base<promise_for<T>> {
    std::optional<T> result;

    task<T> get_return_object() {
        return task<T>{std::coroutine_handle<promise_for>::from_promise(*this)};
    }
    void return_value(T v) { result.emplace(std::move(v)); }
};

template <>
struct promise_for<void> : promise_base<promise_for<void>> {
    task<void> get_return_object();      // 类外定义（需完整 task<void>）
    void return_void() noexcept {}
};

} // namespace async_detail

/** @brief 协程任务（惰性启动；对称转移；void 由 promise 特化支持） */
export template <class T>
class task {
public:
    using promise_type = async_detail::promise_for<T>;
    using handle = std::coroutine_handle<promise_type>;

    task() = default;
    explicit task(handle h) : h_(h) {}
    task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    task& operator=(task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() { if (h_) h_.destroy(); }

    void start() { if (h_ && !h_.done()) h_.resume(); }
    bool done() const { return !h_ || h_.done(); }
    handle release() { return std::exchange(h_, {}); }

    struct awaiter {
        handle h;
        bool await_ready() const noexcept { return false; }
        handle await_suspend(std::coroutine_handle<> cont) {
            h.promise().continuation = cont;
            return h;                     // 对称转移：开始内层
        }
        T await_resume() {
            auto& p = h.promise();
            if (p.error) std::rethrow_exception(p.error);
            if constexpr (!std::is_void_v<T>) return std::move(*p.result);
        }
        ~awaiter() { if (h) h.destroy(); }
    };
    awaiter operator co_await() && { return awaiter{release()}; }

private:
    handle h_{};
};

// promise_for<void>::get_return_object 类外定义（task<void> 此时完整）
inline task<void> async_detail::promise_for<void>::get_return_object() {
    return task<void>{std::coroutine_handle<promise_for<void>>::from_promise(*this)};
}

// ==================== 同步等待 ====================

/** @brief 同步等待（仅适用于不挂起在外部事件上的任务；挂起 → 明确异常） */
export template <class T = void>
T sync_wait(task<T> t) {
    auto h = t.release();
    if (!h) {
        if constexpr (std::is_void_v<T>) return;
        else throw std::runtime_error("cboot.async: empty task");
    }
    h.resume();
    auto& p = h.promise();
    if (!h.done()) {
        h.destroy();
        throw std::runtime_error(
            "cboot.async: task suspended on external event (need an I/O driver)");
    }
    if (p.error) {
        auto e = p.error;
        h.destroy();
        std::rethrow_exception(e);
    }
    if constexpr (std::is_void_v<T>) {
        h.destroy();
    } else {
        T v = std::move(*p.result);
        h.destroy();
        return v;
    }
}

// ==================== 事件桥 ====================

/**
 * @brief 一次性事件（co_await 挂起；set() 恢复全部等待者）
 *
 * I/O 后端集成点：完成通知到达时调用 set()，等待中的连接协程在宿主线程
 * 上被恢复（单线程事件循环语义；多线程后端需保证 set 在所属线程调用）。
 */
export class awaitable_event {
public:
    bool await_ready() const noexcept { return set_; }
    void await_suspend(std::coroutine_handle<> h) { waiters_.push_back(h); }
    void await_resume() const noexcept {}

    /** @brief 触发（幂等）：恢复全部等待者 */
    void set() {
        set_ = true;
        auto ws = std::move(waiters_);
        waiters_.clear();
        for (auto h : ws)
            if (h && !h.done()) h.resume();
    }

    bool is_set() const noexcept { return set_; }

private:
    bool set_ = false;
    std::vector<std::coroutine_handle<>> waiters_;
};

/**
 * @brief 自动重置事件（single-waiter）— I/O 完成 → 连接协程的可重复桥
 *
 * 语义：set() 若有人在等 → 立即恢复并清信号；若无人等 → 保留信号，
 * 下一次 await 立即通过（await_ready 消费后清除）。适用于"每连接一个协程、
 * 每次 I/O 完成后恢复一次"的循环。
 *
 * 线程纪律：set() 在持有连接状态的线程调用；等待者在该线程内同步恢复。
 */
export class async_event {
public:
    bool await_ready() noexcept {
        if (set_) {
            set_ = false;
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept { waiter_ = h; }

    void await_resume() const noexcept {}

    void set() {
        set_ = true;
        if (auto h = std::exchange(waiter_, {}); h && !h.done()) {
            set_ = false;              // 信号直接交给等待者
            h.resume();                // 同步恢复（调用方线程）
        }
    }

    bool is_set() const noexcept { return set_; }

private:
    bool set_ = false;
    std::coroutine_handle<> waiter_{};
};

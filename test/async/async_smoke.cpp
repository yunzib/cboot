// cboot.async 冒烟测试
// 验证：基础 co_return / sync_wait 链式 co_await / void 任务 / 异常传播 /
//       move-only 结果 / 事件桥（外部 set 恢复挂起协程）
import cboot.async;
import std;

task<int> add(int a, int b) { co_return a + b; }

task<int> twice(int x) {
    const int v = co_await add(x, x);       // 对称转移
    co_return v * 2;
}

task<int> nested() {
    const int a = co_await twice(3);        // (3+3)*2 = 12
    const int b = co_await add(a, 1);       // 13
    co_return b;
}

task<void> bump(int& counter) {
    ++counter;
    co_return;
}

// 事件桥任务（参数传递，避免协程 lambda 捕获悬空）
task<void> await_event(awaitable_event& ev, bool& resumed) {
    co_await ev;
    resumed = true;
}

task<int> wait_forever(awaitable_event& ev) {
    co_await ev;
    co_return 1;
}

// auto-reset 事件：两次等待，每次 set 放行一次
task<void> auto_event_wait(async_event& ev, int& state) {
    co_await ev;
    state = 1;
    co_await ev;
    state = 2;
}

task<int> boom() {
    throw std::runtime_error("kaboom");
    co_return 0;
}

task<std::unique_ptr<int>> make_value() {
    co_return std::make_unique<int>(42);
}

int main() {
    // 1. 基础
    if (sync_wait(add(1, 2)) != 3) return 1;

    // 2. 链式 co_await
    if (sync_wait(twice(5)) != 20) return 2;
    if (sync_wait(nested()) != 13) return 3;

    // 3. void 任务
    int counter = 0;
    sync_wait(bump(counter));
    if (counter != 1) return 4;

    // 4. 异常传播
    bool threw = false;
    try {
        (void)sync_wait(boom());
    } catch (const std::runtime_error& e) {
        threw = std::string_view(e.what()) == "kaboom";
    }
    if (!threw) return 5;

    // 5. move-only 结果
    auto p = sync_wait(make_value());
    if (!p || *p != 42) return 6;

    // 6. 事件桥：任务挂起在事件上，外部 set() 恢复
    // 注：协程 lambda 的捕获随闭包临时量销毁（悬空）——必须用参数传递
    awaitable_event ev;
    bool resumed = false;
    auto waiter = await_event(ev, resumed);
    waiter.start();
    if (resumed || waiter.done()) return 7;      // 应挂起
    ev.set();
    if (!resumed || !waiter.done()) return 8;

    // 7. 挂起在外部事件上的任务不能 sync_wait（明确报错）
    awaitable_event never;
    bool reported = false;
    try {
        (void)sync_wait(wait_forever(never));
    } catch (const std::runtime_error&) {
        reported = true;
    }
    if (!reported) return 9;

    // 8. auto-reset 事件：每次 set 恰好放行一次（连接级协程的读/写桥）
    async_event ev2;
    ev2.set();                                   // 先置位 → 首次 await 立即通过
    int state = 0;
    auto waiter2 = auto_event_wait(ev2, state);
    waiter2.start();
    if (state != 1 || waiter2.done()) return 10; // 第一次消费信号，第二次挂起
    ev2.set();                                   // 第二次放行
    if (state != 2 || !waiter2.done()) return 11;

    std::println("ASYNC SMOKE: PASS");
    return 0;
}
